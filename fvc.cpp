// ============================================================================
// fvc.cpp - Single-File Game Mod File Version Controller (Windows / MinGW-w64)
// ============================================================================
// Compile (MinGW-w64):
//   g++ -std=c++20 -O3 -DNDEBUG fvc.cpp -o fvc.exe -static -static-libgcc -static-libstdc++ -s
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace fs = std::filesystem;

// ----------------------------------------------------------------------------
// Binary Ledger Structures (Packed & Sector-Aligned)
// ----------------------------------------------------------------------------
static constexpr uint32_t INVALID_COMMIT = 0xFFFFFFFFu;
static constexpr uint32_t FLAG_FORCE_COPY = 1u << 0; // Isolated copy for mutable configs

#pragma pack(push, 1)
struct CommitHeader {
    uint32_t commit_id;         // 0-based sequential index (byte offset = id * 128)
    uint32_t parent_id;         // Parent commit_id (INVALID_COMMIT for root)
    uint32_t timestamp_ymd;     // Packed sortable date (YYYYMMDD)
    uint32_t file_count;        // Number of FileEntry records in snapshot
    uint64_t epoch_seconds;     // Unix timestamp
    uint64_t manifest_offset;   // Byte offset in .fvc/manifest.bin
    uint32_t added_count;       // Files added relative to parent
    uint32_t modified_count;    // Files modified relative to parent
    uint32_t deleted_count;     // Files deleted relative to parent
    char     message[84];       // Null-terminated UTF-8 commit note (Total: 128 bytes)
};

struct FileEntry {
    uint64_t content_hash;      // 64-bit FNV-1a content hash
    uint64_t file_size;         // Size in bytes
    uint64_t mtime_ticks;       // Win32 FILETIME 100ns ticks
    uint32_t added_date_ymd;    // Sortable date (YYYYMMDD) this version entered repo
    uint32_t flags;             // Bitmask (FLAG_FORCE_COPY, etc.)
    wchar_t  rel_path[240];     // Null-terminated UTF-16 relative path
};
#pragma pack(pop)

static_assert(sizeof(CommitHeader) == 128, "CommitHeader must be exactly 128 bytes");
static_assert(sizeof(FileEntry) == 512, "FileEntry must be exactly 512 bytes");

// ----------------------------------------------------------------------------
// String, Path & DDMMYYYY Date Utilities
// ----------------------------------------------------------------------------
static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

static std::wstring to_lower_wide(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Prepend \\?\ to bypass legacy 260-char MAX_PATH restriction on Win32 calls
static std::wstring to_long_path(const fs::path& p) {
    std::error_code ec;
    fs::path abs_p = fs::absolute(p, ec).lexically_normal();
    std::wstring w = abs_p.wstring();
    if (w.rfind(L"\\\\?\\", 0) != 0) {
        return L"\\\\?\\" + w;
    }
    return w;
}

static std::wstring normalize_rel_path(const fs::path& p) {
    std::wstring w = p.lexically_normal().wstring();
    for (wchar_t& c : w) {
        if (c == L'/') c = L'\\';
    }
    if (w.rfind(L".\\", 0) == 0) {
        w.erase(0, 2);
    }
    return w;
}

static uint32_t filetime_to_ymd(const FILETIME& ft) {
    SYSTEMTIME utc{}, local{};
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    return static_cast<uint32_t>(local.wYear) * 10000u +
           static_cast<uint32_t>(local.wMonth) * 100u +
           static_cast<uint32_t>(local.wDay);
}

static uint32_t current_local_ymd() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    return static_cast<uint32_t>(local.wYear) * 10000u +
           static_cast<uint32_t>(local.wMonth) * 100u +
           static_cast<uint32_t>(local.wDay);
}

static std::string ymd_to_ddmmyyyy(uint32_t ymd) {
    uint32_t y = ymd / 10000u;
    uint32_t m = (ymd / 100u) % 100u;
    uint32_t d = ymd % 100u;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02u%02u%04u", d, m, y);
    return std::string(buf);
}

static bool parse_ddmmyyyy_to_ymd(const std::string& input, uint32_t& out_ymd) {
    std::string digits;
    digits.reserve(8);
    for (char c : input) {
        if (c >= '0' && c <= '9') {
            digits.push_back(c);
        } else if (c != '-' && c != '/' && c != '.') {
            return false;
        }
    }
    if (digits.size() != 8) return false;

    uint32_t day   = static_cast<uint32_t>((digits[0] - '0') * 10 + (digits[1] - '0'));
    uint32_t month = static_cast<uint32_t>((digits[2] - '0') * 10 + (digits[3] - '0'));
    uint32_t year  = static_cast<uint32_t>((digits[4] - '0') * 1000 +
                                           (digits[5] - '0') * 100 +
                                           (digits[6] - '0') * 10 +
                                           (digits[7] - '0'));

    if (year < 1970 || year > 2999) return false;
    if (month < 1 || month > 12) return false;

    static const uint32_t days_in_month[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    uint32_t max_day = days_in_month[month - 1];
    bool is_leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (month == 2 && is_leap) max_day = 29;

    if (day < 1 || day > max_day) return false;

    out_ymd = year * 10000u + month * 100u + day;
    return true;
}

static bool get_file_stat(const fs::path& p, uint64_t& out_size, uint64_t& out_mtime,
                          uint32_t& out_ctime_ymd, uint32_t& out_mtime_ymd) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    std::wstring lp = to_long_path(p);
    if (!GetFileAttributesExW(lp.c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    out_size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    out_mtime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                fad.ftLastWriteTime.dwLowDateTime;
    out_ctime_ymd = filetime_to_ymd(fad.ftCreationTime);
    out_mtime_ymd = filetime_to_ymd(fad.ftLastWriteTime);
    return true;
}

static bool get_ntfs_file_id(const fs::path& p, uint64_t& out_id) {
    std::wstring lp = to_long_path(p);
    HANDLE h = CreateFileW(lp.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    bool ok = GetFileInformationByHandle(h, &info) != 0;
    CloseHandle(h);
    if (ok) {
        out_id = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    }
    return ok;
}

static bool is_mutable_config(const fs::path& p) {
    std::wstring ext = to_lower_wide(p.extension().wstring());
    static const std::unordered_set<std::wstring> copy_exts = {
        L".ini", L".cfg", L".xml", L".json", L".toml", L".txt", L".sav", L".log", L".yaml", L".yml", L".set"
    };
    return copy_exts.find(ext) != copy_exts.end();
}

static std::wstring get_self_rel_path() {
    wchar_t buf[4096];
    DWORD len = GetModuleFileNameW(nullptr, buf, 4096);
    if (len == 0 || len >= 4096) return L"fvc.exe";
    std::error_code ec;
    fs::path rel = fs::relative(fs::path(buf), fs::current_path(), ec);
    if (ec || rel.empty()) return L"fvc.exe";
    return to_lower_wide(normalize_rel_path(rel));
}

static bool should_ignore_path(const std::wstring& norm_rel, const std::wstring& self_lower) {
    if (norm_rel.empty()) return true;
    std::wstring lower = to_lower_wide(norm_rel);
    if (lower == L".fvc" || lower.rfind(L".fvc\\", 0) == 0) return true;
    if (lower == self_lower) return true;
    if (lower == L".fvc.lock") return true;
    return false;
}

// ----------------------------------------------------------------------------
// Fast, Handle-Safe Win32 Directory Tree Scanner
// ----------------------------------------------------------------------------
// Collects all files & directories in memory and closes all FindFirstFileExW
// handles BEFORE any caller deletes or modifies files on disk.
struct ScannedFile {
    fs::path     full_path;
    std::wstring rel_path;
    std::wstring key_lower;
    uint64_t     file_size;
    uint64_t     mtime_ticks;
    uint32_t     ctime_ymd;
    uint32_t     mtime_ymd;
};

static void scan_working_tree(const std::wstring& self_lower,
                              std::vector<ScannedFile>& out_files,
                              std::vector<fs::path>& out_dirs) {
    out_files.clear();
    out_dirs.clear();

    fs::path root_abs = fs::current_path();
    std::vector<std::wstring> dir_stack;
    dir_stack.push_back(L"");

    while (!dir_stack.empty()) {
        std::wstring rel_dir = std::move(dir_stack.back());
        dir_stack.pop_back();

        fs::path cur_dir = rel_dir.empty() ? root_abs : (root_abs / rel_dir);
        std::wstring search_pattern = to_long_path(cur_dir);
        if (!search_pattern.empty() && search_pattern.back() != L'\\') {
            search_pattern.push_back(L'\\');
        }
        search_pattern.push_back(L'*');

        WIN32_FIND_DATAW ffd{};
        HANDLE hFind = FindFirstFileExW(search_pattern.c_str(),
                                        FindExInfoBasic,
                                        &ffd,
                                        FindExSearchNameMatch,
                                        nullptr,
                                        FIND_FIRST_EX_LARGE_FETCH);
        if (hFind == INVALID_HANDLE_VALUE) continue;

        do {
            if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) {
                continue;
            }

            std::wstring rel_item = rel_dir.empty()
                                    ? std::wstring(ffd.cFileName)
                                    : (rel_dir + L"\\" + ffd.cFileName);

            if (should_ignore_path(rel_item, self_lower)) {
                continue;
            }

            fs::path full_item = root_abs / rel_item;

            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // Skip NTFS junctions/symlinks to avoid recursion cycles
                if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                    out_dirs.push_back(full_item);
                    dir_stack.push_back(rel_item);
                }
            } else {
                ScannedFile sf{};
                sf.full_path   = std::move(full_item);
                sf.rel_path    = std::move(rel_item);
                sf.key_lower   = to_lower_wide(sf.rel_path);
                sf.file_size   = (static_cast<uint64_t>(ffd.nFileSizeHigh) << 32) | ffd.nFileSizeLow;
                sf.mtime_ticks = (static_cast<uint64_t>(ffd.ftLastWriteTime.dwHighDateTime) << 32) |
                                 ffd.ftLastWriteTime.dwLowDateTime;
                sf.ctime_ymd   = filetime_to_ymd(ffd.ftCreationTime);
                sf.mtime_ymd   = filetime_to_ymd(ffd.ftLastWriteTime);
                out_files.push_back(std::move(sf));
            }
        } while (FindNextFileW(hFind, &ffd) != 0);

        FindClose(hFind);
    }

    std::sort(out_files.begin(), out_files.end(), [](const ScannedFile& a, const ScannedFile& b) {
        return _wcsicmp(a.rel_path.c_str(), b.rel_path.c_str()) < 0;
    });
}

// ----------------------------------------------------------------------------
// Safety Guards: NTFS Check, Running Game Lock Detection & Transaction Lock
// ----------------------------------------------------------------------------
static bool verify_ntfs_volume() {
    wchar_t vol_path[MAX_PATH];
    if (!GetVolumePathNameW(fs::current_path().c_str(), vol_path, MAX_PATH)) {
        return false;
    }
    wchar_t fs_name[MAX_PATH] = {0};
    if (!GetVolumeInformationW(vol_path, nullptr, 0, nullptr, nullptr, nullptr, fs_name, MAX_PATH)) {
        return false;
    }
    return _wcsicmp(fs_name, L"NTFS") == 0;
}

static bool ensure_game_not_running(const std::wstring& self_lower) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(L".", ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::wstring rel = normalize_rel_path(fs::relative(entry.path(), L".", ec));
        std::wstring lower = to_lower_wide(rel);
        if (lower == self_lower) continue;
        if (to_lower_wide(entry.path().extension().wstring()) != L".exe") continue;

        std::wstring lp = to_long_path(entry.path());
        HANDLE h = CreateFileW(lp.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
                std::fprintf(stderr, "Error: Game executable '%s' is currently running or locked.\n"
                                     "Close the game and mod managers before modifying the working tree.\n",
                             wide_to_utf8(rel).c_str());
                return false;
            }
        } else {
            CloseHandle(h);
        }
    }
    return true;
}

struct ScopedRepoLock {
    bool active = false;
    explicit ScopedRepoLock(bool enable) : active(enable) {
        if (!active) return;
        std::error_code ec;
        if (fs::exists(L".fvc/LOCK", ec)) {
            std::fprintf(stderr, "Warning: Stale .fvc/LOCK detected from an interrupted operation.\n");
        }
        std::ofstream f(".fvc/LOCK", std::ios::binary | std::ios::trunc);
    }
    ~ScopedRepoLock() {
        if (!active) return;
        std::error_code ec;
        fs::remove(L".fvc/LOCK", ec);
    }
};

// ----------------------------------------------------------------------------
// Zero-Copy Memory-Mapped Hashing & CAS Hard-Link Engine
// ----------------------------------------------------------------------------
static bool hash_file_mmap(const fs::path& file_path, uint64_t& out_hash) {
    std::wstring lp = to_long_path(file_path);
    HANDLE hFile = CreateFileW(lp.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(hFile, &sz)) {
        CloseHandle(hFile);
        return false;
    }

    uint64_t hash = 14695981039346656037ULL; // FNV-1a 64-bit offset basis
    if (sz.QuadPart == 0) {
        CloseHandle(hFile);
        out_hash = hash;
        return true;
    }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        return false;
    }

    constexpr uint64_t CHUNK_SIZE = 64ULL * 1024ULL * 1024ULL;
    uint64_t remaining = static_cast<uint64_t>(sz.QuadPart);
    uint64_t offset = 0;

    while (remaining > 0) {
        SIZE_T view_size = static_cast<SIZE_T>(std::min<uint64_t>(remaining, CHUNK_SIZE));
        DWORD off_high = static_cast<DWORD>(offset >> 32);
        DWORD off_low  = static_cast<DWORD>(offset & 0xFFFFFFFFULL);

        const uint8_t* data = static_cast<const uint8_t*>(
            MapViewOfFile(hMap, FILE_MAP_READ, off_high, off_low, view_size));
        if (!data) {
            CloseHandle(hMap);
            CloseHandle(hFile);
            return false;
        }
        for (SIZE_T i = 0; i < view_size; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ULL;
        }
        UnmapViewOfFile(data);
        offset += view_size;
        remaining -= view_size;
    }

    CloseHandle(hMap);
    CloseHandle(hFile);
    out_hash = hash;
    return true;
}

static fs::path get_blob_path(uint64_t hash) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02x/%016llx",
                  static_cast<unsigned>(hash & 0xFF),
                  static_cast<unsigned long long>(hash));
    return fs::path(L".fvc/objects") / buf;
}

static bool link_or_copy_file(const fs::path& src_path, const fs::path& dst_path, bool force_copy) {
    std::error_code ec;
    fs::create_directories(dst_path.parent_path(), ec);

    std::wstring src = to_long_path(src_path);
    std::wstring dst = to_long_path(dst_path);

    if (fs::exists(dst_path, ec)) {
        SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileW(dst.c_str())) {
            return false;
        }
    }

    if (!force_copy) {
        if (CreateHardLinkW(dst.c_str(), src.c_str(), nullptr)) {
            return true;
        }
    }
    return CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0;
}

static bool store_blob_if_missing(const fs::path& work_file, uint64_t hash, bool force_copy) {
    fs::path blob = get_blob_path(hash);
    std::error_code ec;
    if (fs::exists(blob, ec)) {
        uint64_t work_id = 0, blob_id = 0;
        if (!force_copy && get_ntfs_file_id(work_file, work_id) && get_ntfs_file_id(blob, blob_id)) {
            if (work_id != blob_id) {
                link_or_copy_file(blob, work_file, false);
            }
        }
        return true;
    }
    return link_or_copy_file(work_file, blob, force_copy);
}

// ----------------------------------------------------------------------------
// Ledger & Manifest Persistence
// ----------------------------------------------------------------------------
static bool is_repo_initialized() {
    std::error_code ec;
    return fs::exists(L".fvc/commits.bin", ec) && fs::exists(L".fvc/manifest.bin", ec);
}

static uint32_t get_total_commits() {
    std::ifstream f(".fvc/commits.bin", std::ios::binary | std::ios::ate);
    if (!f) return 0;
    return static_cast<uint32_t>(static_cast<uint64_t>(f.tellg()) / sizeof(CommitHeader));
}

static uint32_t read_head() {
    std::ifstream f(".fvc/HEAD", std::ios::binary);
    uint32_t head = INVALID_COMMIT;
    if (f) f.read(reinterpret_cast<char*>(&head), sizeof(head));
    return head;
}

static bool write_head(uint32_t commit_id) {
    std::ofstream f(".fvc/HEAD", std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(&commit_id), sizeof(commit_id));
    return f.good();
}

static bool load_commit_header(uint32_t commit_id, CommitHeader& out_hdr) {
    std::ifstream f(".fvc/commits.bin", std::ios::binary | std::ios::ate);
    if (!f) return false;
    uint64_t total_bytes = static_cast<uint64_t>(f.tellg());
    uint64_t offset = static_cast<uint64_t>(commit_id) * sizeof(CommitHeader);
    if (offset + sizeof(CommitHeader) > total_bytes) return false;

    f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    f.read(reinterpret_cast<char*>(&out_hdr), sizeof(CommitHeader));
    return f.good();
}

static bool load_manifest(const CommitHeader& hdr, std::vector<FileEntry>& out_entries) {
    out_entries.resize(hdr.file_count);
    if (hdr.file_count == 0) return true;

    std::ifstream f(".fvc/manifest.bin", std::ios::binary);
    if (!f) return false;
    f.seekg(static_cast<std::streamoff>(hdr.manifest_offset), std::ios::beg);
    f.read(reinterpret_cast<char*>(out_entries.data()),
           static_cast<std::streamsize>(hdr.file_count) * sizeof(FileEntry));
    return f.good();
}

static void prune_empty_directories(std::vector<fs::path>& dirs) {
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return a.wstring().size() > b.wstring().size();
    });
    for (const auto& d : dirs) {
        RemoveDirectoryW(to_long_path(d).c_str());
    }
}

// ----------------------------------------------------------------------------
// Command 1: COMMIT (Also called automatically by INIT for Commit #0)
// ----------------------------------------------------------------------------
static int cmd_commit(const std::string& message, bool allow_empty = false) {
    if (!is_repo_initialized()) {
        std::fprintf(stderr, "Error: Repository not initialized. Run 'fvc init' first.\n");
        return 1;
    }

    ScopedRepoLock lock(true);
    std::wstring self_lower = get_self_rel_path();
    uint32_t parent_id = read_head();

    std::unordered_map<std::wstring, FileEntry> parent_map;
    if (parent_id != INVALID_COMMIT) {
        CommitHeader p_hdr{};
        std::vector<FileEntry> p_entries;
        if (load_commit_header(parent_id, p_hdr) && load_manifest(p_hdr, p_entries)) {
            parent_map.reserve(p_entries.size());
            for (const auto& e : p_entries) {
                parent_map[to_lower_wide(e.rel_path)] = e;
            }
        }
    }

    std::vector<ScannedFile> scanned_files;
    std::vector<fs::path> scanned_dirs;
    scan_working_tree(self_lower, scanned_files, scanned_dirs);

    std::vector<FileEntry> new_manifest;
    new_manifest.reserve(scanned_files.size());
    std::unordered_set<std::wstring> seen_keys;
    seen_keys.reserve(scanned_files.size());

    uint32_t added_count = 0;
    uint32_t modified_count = 0;
    uint32_t today_ymd = current_local_ymd();

    for (const auto& sf : scanned_files) {
        if (sf.rel_path.size() >= 240) {
            std::fprintf(stderr, "Error: Relative path exceeds 239 chars: %s\n",
                         wide_to_utf8(sf.rel_path).c_str());
            return 1;
        }

        seen_keys.insert(sf.key_lower);

        FileEntry entry{};
        entry.file_size = sf.file_size;
        entry.mtime_ticks = sf.mtime_ticks;
        entry.flags = is_mutable_config(sf.full_path) ? FLAG_FORCE_COPY : 0u;
        std::wcsncpy(entry.rel_path, sf.rel_path.c_str(), 239);

        auto p_it = parent_map.find(sf.key_lower);
        if (p_it != parent_map.end() &&
            p_it->second.file_size == sf.file_size &&
            p_it->second.mtime_ticks == sf.mtime_ticks) {
            entry.content_hash = p_it->second.content_hash;
            entry.added_date_ymd = p_it->second.added_date_ymd;
        } else {
            if (!hash_file_mmap(sf.full_path, entry.content_hash)) {
                std::fprintf(stderr, "Error hashing file: %s\n", wide_to_utf8(sf.rel_path).c_str());
                return 1;
            }
            if (!store_blob_if_missing(sf.full_path, entry.content_hash, (entry.flags & FLAG_FORCE_COPY) != 0)) {
                std::fprintf(stderr, "Error storing blob for: %s\n", wide_to_utf8(sf.rel_path).c_str());
                return 1;
            }

            if (p_it == parent_map.end()) {
                uint32_t best_date = std::max(sf.ctime_ymd, sf.mtime_ymd);
                entry.added_date_ymd = (best_date > 0) ? best_date : today_ymd;
                ++added_count;
            } else if (p_it->second.content_hash != entry.content_hash) {
                entry.added_date_ymd = today_ymd;
                ++modified_count;
            } else {
                entry.added_date_ymd = p_it->second.added_date_ymd;
            }
        }

        new_manifest.push_back(entry);
    }

    uint32_t deleted_count = 0;
    for (const auto& kv : parent_map) {
        if (seen_keys.find(kv.first) == seen_keys.end()) {
            ++deleted_count;
        }
    }

    if (!allow_empty && parent_id != INVALID_COMMIT &&
        added_count == 0 && modified_count == 0 && deleted_count == 0) {
        std::printf("Nothing to commit (working tree matches Commit #%u).\n", parent_id);
        return 0;
    }

    std::ofstream mf(".fvc/manifest.bin", std::ios::binary | std::ios::app | std::ios::ate);
    if (!mf) {
        std::fprintf(stderr, "Error opening .fvc/manifest.bin for writing.\n");
        return 1;
    }
    uint64_t manifest_offset = static_cast<uint64_t>(mf.tellp());
    if (!new_manifest.empty()) {
        mf.write(reinterpret_cast<const char*>(new_manifest.data()),
                 static_cast<std::streamsize>(new_manifest.size()) * sizeof(FileEntry));
    }
    mf.close();

    uint32_t new_commit_id = get_total_commits();
    CommitHeader hdr{};
    hdr.commit_id = new_commit_id;
    hdr.parent_id = parent_id;
    hdr.timestamp_ymd = today_ymd;
    hdr.file_count = static_cast<uint32_t>(new_manifest.size());
    hdr.epoch_seconds = static_cast<uint64_t>(std::time(nullptr));
    hdr.manifest_offset = manifest_offset;
    hdr.added_count = added_count;
    hdr.modified_count = modified_count;
    hdr.deleted_count = deleted_count;
    std::strncpy(hdr.message, message.c_str(), sizeof(hdr.message) - 1);

    std::ofstream cf(".fvc/commits.bin", std::ios::binary | std::ios::app);
    cf.write(reinterpret_cast<const char*>(&hdr), sizeof(CommitHeader));
    cf.close();

    write_head(new_commit_id);

    std::printf("Committed #%u on %s (\"%s\")\n",
                new_commit_id, ymd_to_ddmmyyyy(today_ymd).c_str(), hdr.message);
    std::printf("Changes: +%u added, ~%u modified, -%u deleted (%u total files tracked).\n",
                added_count, modified_count, deleted_count, hdr.file_count);
    return 0;
}

// ----------------------------------------------------------------------------
// Command 2: INIT (Verify NTFS, create .fvc, and snapshot Commit #0)
// ----------------------------------------------------------------------------
static int cmd_init() {
    if (is_repo_initialized()) {
        std::fprintf(stderr, "Repository already initialized in .fvc/\n");
        return 1;
    }
    if (!verify_ntfs_volume()) {
        std::fprintf(stderr, "Error: Current drive is not formatted as NTFS. Hard-link CAS requires NTFS.\n");
        return 1;
    }

    std::error_code ec;
    fs::create_directories(L".fvc/objects", ec);
    SetFileAttributesW(to_long_path(L".fvc").c_str(), FILE_ATTRIBUTE_HIDDEN);

    {
        std::ofstream cf(".fvc/commits.bin", std::ios::binary | std::ios::trunc);
        std::ofstream mf(".fvc/manifest.bin", std::ios::binary | std::ios::trunc);
    }
    write_head(INVALID_COMMIT);

    std::printf("Initialized empty FVC repository in .fvc/\n");
    std::printf("Creating initial baseline snapshot (Commit #0)...\n");
    return cmd_commit("Initial vanilla baseline", true);
}

// ----------------------------------------------------------------------------
// Command 3: STATUS (Inspect uncommitted working-tree changes)
// ----------------------------------------------------------------------------
static int cmd_status() {
    if (!is_repo_initialized()) {
        std::fprintf(stderr, "Error: Repository not initialized. Run 'fvc init' first.\n");
        return 1;
    }

    uint32_t head_id = read_head();
    CommitHeader hdr{};
    std::vector<FileEntry> entries;
    if (head_id == INVALID_COMMIT || !load_commit_header(head_id, hdr) || !load_manifest(hdr, entries)) {
        std::fprintf(stderr, "Error loading active HEAD commit.\n");
        return 1;
    }

    std::unordered_map<std::wstring, const FileEntry*> head_map;
    for (const auto& e : entries) {
        head_map[to_lower_wide(e.rel_path)] = &e;
    }

    std::wstring self_lower = get_self_rel_path();
    std::vector<ScannedFile> scanned_files;
    std::vector<fs::path> scanned_dirs;
    scan_working_tree(self_lower, scanned_files, scanned_dirs);

    std::unordered_set<std::wstring> seen_keys;
    uint32_t added = 0, modified = 0, deleted = 0;

    std::printf("On Commit #%u (\"%s\")\n", hdr.commit_id, hdr.message);

    for (const auto& sf : scanned_files) {
        seen_keys.insert(sf.key_lower);

        auto m_it = head_map.find(sf.key_lower);
        if (m_it == head_map.end()) {
            std::printf("  [+] %s\n", wide_to_utf8(sf.rel_path).c_str());
            ++added;
        } else {
            const FileEntry* ref = m_it->second;
            if (ref->file_size != sf.file_size || ref->mtime_ticks != sf.mtime_ticks) {
                uint64_t h = 0;
                if (hash_file_mmap(sf.full_path, h) && h != ref->content_hash) {
                    std::printf("  [~] %s\n", wide_to_utf8(sf.rel_path).c_str());
                    ++modified;
                }
            }
        }
    }

    for (const auto& e : entries) {
        if (seen_keys.find(to_lower_wide(e.rel_path)) == seen_keys.end()) {
            std::printf("  [-] %s\n", wide_to_utf8(e.rel_path).c_str());
            ++deleted;
        }
    }

    if (added == 0 && modified == 0 && deleted == 0) {
        std::printf("Working tree is clean.\n");
    } else {
        std::printf("Summary: +%u added, ~%u modified, -%u deleted uncommitted.\n",
                    added, modified, deleted);
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Command 4: HISTORY (List all commits & active HEAD marker)
// ----------------------------------------------------------------------------
static int cmd_history(int limit = 0) {
    if (!is_repo_initialized()) {
        std::fprintf(stderr, "Error: Repository not initialized. Run 'fvc init' first.\n");
        return 1;
    }

    uint32_t total = get_total_commits();
    uint32_t current_head = read_head();

    std::printf("%-8s %-6s %-8s %-19s %-18s %-7s %s\n",
                "COMMIT", "STATE", "PARENT", "TIMESTAMP", "CHANGES (+/~/-)", "FILES", "MESSAGE");
    std::printf("------------------------------------------------------------------------------------------------\n");

    int printed = 0;
    for (int64_t i = static_cast<int64_t>(total) - 1; i >= 0; --i) {
        if (limit > 0 && printed >= limit) break;

        CommitHeader hdr{};
        if (!load_commit_header(static_cast<uint32_t>(i), hdr)) continue;

        std::time_t t = static_cast<std::time_t>(hdr.epoch_seconds);
        std::tm local_tm{};
        localtime_s(&local_tm, &t);
        char time_buf[32];
        std::strftime(time_buf, sizeof(time_buf), "%d-%m-%Y %H:%M:%S", &local_tm);

        char delta_buf[32];
        std::snprintf(delta_buf, sizeof(delta_buf), "+%u / ~%u / -%u",
                      hdr.added_count, hdr.modified_count, hdr.deleted_count);

        char parent_buf[16];
        if (hdr.parent_id == INVALID_COMMIT) {
            std::snprintf(parent_buf, sizeof(parent_buf), "NONE");
        } else {
            std::snprintf(parent_buf, sizeof(parent_buf), "#%u", hdr.parent_id);
        }

        const char* marker = (hdr.commit_id == current_head) ? "*HEAD" : "";
        std::printf("#%-7u %-6s %-8s %-19s %-18s %-7u %s\n",
                    hdr.commit_id, marker, parent_buf, time_buf, delta_buf, hdr.file_count, hdr.message);
        ++printed;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Command 5: SHOW (Inspect exact file diff for a specific commit ID)
// ----------------------------------------------------------------------------
static int cmd_show(uint32_t commit_id) {
    if (!is_repo_initialized()) {
        std::fprintf(stderr, "Error: Repository not initialized. Run 'fvc init' first.\n");
        return 1;
    }

    CommitHeader hdr{};
    if (!load_commit_header(commit_id, hdr)) {
        std::fprintf(stderr, "Error: Commit #%u does not exist.\n", commit_id);
        return 1;
    }

    std::vector<FileEntry> cur_files, parent_files;
    load_manifest(hdr, cur_files);

    std::unordered_map<std::wstring, uint64_t> parent_map;
    if (hdr.parent_id != INVALID_COMMIT) {
        CommitHeader p_hdr{};
        if (load_commit_header(hdr.parent_id, p_hdr) && load_manifest(p_hdr, parent_files)) {
            for (const auto& e : parent_files) {
                parent_map[to_lower_wide(e.rel_path)] = e.content_hash;
            }
        }
    }

    std::printf("Commit #%u | Date: %s | Message: \"%s\"\n",
                hdr.commit_id, ymd_to_ddmmyyyy(hdr.timestamp_ymd).c_str(), hdr.message);
    std::printf("Changes: +%u added, ~%u modified, -%u deleted\n",
                hdr.added_count, hdr.modified_count, hdr.deleted_count);

    std::unordered_set<std::wstring> cur_keys;
    for (const auto& e : cur_files) {
        std::wstring key = to_lower_wide(e.rel_path);
        cur_keys.insert(key);
        auto it = parent_map.find(key);
        if (it == parent_map.end()) {
            std::printf("  [+] %s\n", wide_to_utf8(e.rel_path).c_str());
        } else if (it->second != e.content_hash) {
            std::printf("  [~] %s\n", wide_to_utf8(e.rel_path).c_str());
        }
    }
    for (const auto& e : parent_files) {
        if (cur_keys.find(to_lower_wide(e.rel_path)) == cur_keys.end()) {
            std::printf("  [-] %s\n", wide_to_utf8(e.rel_path).c_str());
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Command 6: REVERT (Restore working tree to HEAD or specific Commit ID)
// ----------------------------------------------------------------------------
static int cmd_revert(uint32_t target_commit_id) {
    if (!is_repo_initialized()) {
        std::fprintf(stderr, "Error: Repository not initialized. Run 'fvc init' first.\n");
        return 1;
    }

    if (target_commit_id == INVALID_COMMIT) {
        target_commit_id = read_head();
        if (target_commit_id == INVALID_COMMIT) {
            std::fprintf(stderr, "Error: Repository has no active HEAD.\n");
            return 1;
        }
    }

    CommitHeader target_hdr{};
    if (!load_commit_header(target_commit_id, target_hdr)) {
        std::fprintf(stderr, "Error: Commit #%u not found in ledger.\n", target_commit_id);
        return 1;
    }

    std::vector<FileEntry> target_entries;
    if (!load_manifest(target_hdr, target_entries)) {
        std::fprintf(stderr, "Error reading manifest for Commit #%u.\n", target_commit_id);
        return 1;
    }

    std::wstring self_lower = get_self_rel_path();
    if (!ensure_game_not_running(self_lower)) {
        return 1;
    }

    ScopedRepoLock lock(true);

    std::unordered_map<std::wstring, const FileEntry*> target_map;
    target_map.reserve(target_entries.size());
    for (const auto& e : target_entries) {
        target_map[to_lower_wide(e.rel_path)] = &e;
    }

    // Phase 0: Scan working tree completely before mutating disk
    std::vector<ScannedFile> scanned_files;
    std::vector<fs::path> dir_candidates;
    scan_working_tree(self_lower, scanned_files, dir_candidates);

    uint32_t removed_files = 0;
    uint32_t restored_files = 0;
    uint32_t intact_files = 0;
    uint32_t failed_files = 0;

    // Pass 1: Delete untracked files not in target_manifest
    for (const auto& sf : scanned_files) {
        if (target_map.find(sf.key_lower) == target_map.end()) {
            std::wstring lp = to_long_path(sf.full_path);
            SetFileAttributesW(lp.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(lp.c_str())) {
                ++removed_files;
            } else {
                std::fprintf(stderr, "  [Failed Delete (Win32 Err %lu)] %s\n",
                             GetLastError(), wide_to_utf8(sf.rel_path).c_str());
                ++failed_files;
            }
        }
    }

    // Pass 2: Verify or restore every tracked file in target_manifest
    std::error_code ec;
    for (const auto& entry : target_entries) {
        fs::path work_path = fs::path(L".") / entry.rel_path;
        fs::path blob_path = get_blob_path(entry.content_hash);
        bool force_copy = (entry.flags & FLAG_FORCE_COPY) != 0;

        bool needs_restore = true;
        if (fs::exists(work_path, ec)) {
            uint64_t fsize = 0, fmtime = 0;
            uint32_t c_ymd = 0, m_ymd = 0;
            if (get_file_stat(work_path, fsize, fmtime, c_ymd, m_ymd) && fsize == entry.file_size) {
                if (!force_copy) {
                    uint64_t work_id = 0, blob_id = 0;
                    if (get_ntfs_file_id(work_path, work_id) &&
                        get_ntfs_file_id(blob_path, blob_id) &&
                        work_id == blob_id &&
                        fmtime == entry.mtime_ticks) {
                        needs_restore = false;
                    }
                } else {
                    uint64_t cur_hash = 0;
                    if (hash_file_mmap(work_path, cur_hash) && cur_hash == entry.content_hash) {
                        needs_restore = false;
                    }
                }
            }
        }

        if (needs_restore) {
            if (!link_or_copy_file(blob_path, work_path, force_copy)) {
                std::fprintf(stderr, "Error restoring file (Win32 Err %lu): %s\n",
                             GetLastError(), wide_to_utf8(entry.rel_path).c_str());
                ++failed_files;
            } else {
                ++restored_files;
            }
        } else {
            ++intact_files;
        }
    }

    // Pass 3: Remove empty orphaned mod directories bottom-up
    prune_empty_directories(dir_candidates);

    write_head(target_commit_id);
    std::printf("Reverted working tree to Commit #%u (\"%s\")\n",
                target_commit_id, target_hdr.message);
    std::printf("Summary: %u restored, %u untracked removed, %u already intact",
                restored_files, removed_files, intact_files);
    if (failed_files > 0) {
        std::printf(", %u failed (Try running terminal as Administrator)", failed_files);
    }
    std::printf(".\n");
    return (failed_files == 0) ? 0 : 1;
}

// ----------------------------------------------------------------------------
// Command 7: DATE AUDIT (Works with OR without .fvc repo initialization)
// ----------------------------------------------------------------------------
static int cmd_date_query(uint32_t target_ymd) {
    bool has_repo = is_repo_initialized();
    std::string disp_date = ymd_to_ddmmyyyy(target_ymd);

    std::vector<FileEntry> head_entries;
    std::unordered_set<std::wstring> printed_keys;

    if (has_repo) {
        uint32_t head_id = read_head();
        CommitHeader hdr{};
        if (head_id != INVALID_COMMIT && load_commit_header(head_id, hdr)) {
            load_manifest(hdr, head_entries);
        }
        std::printf("Scanning repository + live filesystem metadata for date %s:\n", disp_date.c_str());
    } else {
        std::printf("[Metadata Fallback Mode: No .fvc repo — scanning folder metadata for %s]\n",
                    disp_date.c_str());
    }

    uint32_t match_count = 0;

    // 1. Check committed manifest entries if repository exists
    for (const auto& e : head_entries) {
        if (e.added_date_ymd == target_ymd) {
            std::wstring key = to_lower_wide(e.rel_path);
            printed_keys.insert(key);
            std::printf("  [Committed]     %s (%llu bytes)\n",
                        wide_to_utf8(e.rel_path).c_str(),
                        static_cast<unsigned long long>(e.file_size));
            ++match_count;
        }
    }

    // 2. Scan live directory tree metadata
    std::wstring self_lower = get_self_rel_path();
    std::vector<ScannedFile> scanned_files;
    std::vector<fs::path> scanned_dirs;
    scan_working_tree(self_lower, scanned_files, scanned_dirs);

    for (const auto& sf : scanned_files) {
        if (printed_keys.find(sf.key_lower) != printed_keys.end()) continue;

        if (sf.ctime_ymd == target_ymd || sf.mtime_ymd == target_ymd) {
            const char* tag = "[Modified]     ";
            if (sf.ctime_ymd == target_ymd && sf.mtime_ymd != target_ymd) {
                tag = "[Extracted/New]";
            } else if (sf.ctime_ymd == target_ymd && sf.mtime_ymd == target_ymd) {
                tag = "[Added/Created]";
            }

            std::printf("  %s %s (Created: %s | Modified: %s | %llu bytes)\n",
                        tag,
                        wide_to_utf8(sf.rel_path).c_str(),
                        ymd_to_ddmmyyyy(sf.ctime_ymd).c_str(),
                        ymd_to_ddmmyyyy(sf.mtime_ymd).c_str(),
                        static_cast<unsigned long long>(sf.file_size));
            ++match_count;
        }
    }

    if (match_count == 0) {
        std::printf("  (No files found for date %s)\n", disp_date.c_str());
    } else {
        std::printf("Total matched files on %s: %u\n", disp_date.c_str(), match_count);
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Command 8: PURGE DATE (Works with OR without .fvc repo initialization)
// ----------------------------------------------------------------------------
static int cmd_purge_date(uint32_t target_ymd) {
    std::wstring self_lower = get_self_rel_path();
    if (!ensure_game_not_running(self_lower)) {
        return 1;
    }

    bool has_repo = is_repo_initialized();
    ScopedRepoLock lock(has_repo);
    std::string disp_date = ymd_to_ddmmyyyy(target_ymd);

    std::unordered_map<std::wstring, FileEntry> fallback_map;
    std::unordered_map<std::wstring, FileEntry> head_map;

    if (has_repo) {
        uint32_t total_commits = get_total_commits();
        for (int64_t i = static_cast<int64_t>(total_commits) - 1; i >= 0; --i) {
            CommitHeader c_hdr{};
            if (load_commit_header(static_cast<uint32_t>(i), c_hdr) && c_hdr.timestamp_ymd < target_ymd) {
                std::vector<FileEntry> fb_entries;
                if (load_manifest(c_hdr, fb_entries)) {
                    for (const auto& e : fb_entries) {
                        fallback_map[to_lower_wide(e.rel_path)] = e;
                    }
                }
                break;
            }
        }

        uint32_t head_id = read_head();
        if (head_id != INVALID_COMMIT) {
            CommitHeader h_hdr{};
            std::vector<FileEntry> h_entries;
            if (load_commit_header(head_id, h_hdr) && load_manifest(h_hdr, h_entries)) {
                for (const auto& e : h_entries) {
                    std::wstring key = to_lower_wide(e.rel_path);
                    head_map[key] = e;
                    if (e.added_date_ymd < target_ymd && fallback_map.find(key) == fallback_map.end()) {
                        fallback_map[key] = e;
                    }
                }
            }
        }
    } else {
        std::printf("[Metadata Fallback Mode: No .fvc repo — purging files matching date %s]\n",
                    disp_date.c_str());
    }

    // Phase 1: Collect all files & directories into memory first (closes all FindHandles)
    std::vector<ScannedFile> scanned_files;
    std::vector<fs::path> dir_candidates;
    scan_working_tree(self_lower, scanned_files, dir_candidates);

    uint32_t deleted_count = 0;
    uint32_t restored_count = 0;
    uint32_t failed_count = 0;

    // Phase 2: Execute deletions/restorations safely over the in-memory list
    for (const auto& sf : scanned_files) {
        bool touched_on_date = (sf.ctime_ymd == target_ymd || sf.mtime_ymd == target_ymd);

        if (has_repo) {
            auto h_it = head_map.find(sf.key_lower);
            if (h_it != head_map.end() && h_it->second.added_date_ymd == target_ymd) {
                touched_on_date = true;
            }
        }

        if (!touched_on_date) continue;

        auto fb_it = fallback_map.find(sf.key_lower);
        if (has_repo && fb_it != fallback_map.end()) {
            const FileEntry& old_entry = fb_it->second;
            fs::path blob_path = get_blob_path(old_entry.content_hash);
            bool force_copy = (old_entry.flags & FLAG_FORCE_COPY) != 0;
            if (link_or_copy_file(blob_path, sf.full_path, force_copy)) {
                std::printf("  [Restored Prior Version] %s\n", wide_to_utf8(sf.rel_path).c_str());
                ++restored_count;
            } else {
                std::fprintf(stderr, "  [Failed Restore (Err %lu)] %s\n",
                             GetLastError(), wide_to_utf8(sf.rel_path).c_str());
                ++failed_count;
            }
        } else {
            std::wstring lp = to_long_path(sf.full_path);
            SetFileAttributesW(lp.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(lp.c_str())) {
                std::printf("  [Deleted]                %s\n", wide_to_utf8(sf.rel_path).c_str());
                ++deleted_count;
            } else {
                DWORD err = GetLastError();
                std::fprintf(stderr, "  [Failed Delete (Err %lu)]  %s\n",
                             err, wide_to_utf8(sf.rel_path).c_str());
                ++failed_count;
            }
        }
    }

    // Phase 3: Prune empty directories bottom-up
    prune_empty_directories(dir_candidates);

    if (has_repo) {
        std::printf("Purge for %s complete: %u net-new files deleted, %u overwritten files restored",
                    disp_date.c_str(), deleted_count, restored_count);
    } else {
        std::printf("Metadata purge for %s complete: %u files deleted",
                    disp_date.c_str(), deleted_count);
    }
    if (failed_count > 0) {
        std::printf(", %u failed (Run PowerShell as Administrator for C:\\Program Files (x86))",
                    failed_count);
    }
    std::printf(".\n");
    return (failed_count == 0) ? 0 : 1;
}

// ----------------------------------------------------------------------------
// CLI Argument Parser & Entry Point
// ----------------------------------------------------------------------------
static void print_usage() {
    std::printf(
        "FVC - Game Mod File Version Controller (NTFS Hard-Link CAS)\n"
        "Usage:\n"
        "  fvc init                        Initialize repo and snapshot Commit #0\n"
        "  fvc status                      Show uncommitted changes in game folder\n"
        "  fvc commit \"<message>\"          Snapshot current working tree state\n"
        "  fvc history [limit]             Show commit history and active *HEAD\n"
        "  fvc show <commit_id>            Show exact files changed in a commit\n"
        "  fvc revert [commit_id]          Revert to HEAD or jump to specific commit ID\n"
        "  fvc date <DDMMYYYY>             List files added/modified on date (no init required)\n"
        "  fvc purge <DDMMYYYY>            Delete files from date (no init required)\n"
    );
}

static bool parse_uint32_arg(const std::string& s, uint32_t& out_val) {
    if (s.empty()) return false;
    char* endp = nullptr;
    unsigned long val = std::strtoul(s.c_str(), &endp, 10);
    if (!endp || *endp != '\0') return false;
    out_val = static_cast<uint32_t>(val);
    return true;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    LPWSTR* argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv_w || argc < 2) {
        if (argv_w) LocalFree(argv_w);
        print_usage();
        return 0;
    }

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.push_back(wide_to_utf8(argv_w[i]));
    }
    LocalFree(argv_w);

    const std::string& cmd = args[1];

    if (cmd == "init") {
        return cmd_init();
    }
    if (cmd == "status") {
        return cmd_status();
    }
    if (cmd == "commit") {
        std::string msg = (argc >= 3) ? args[2] : "Mod snapshot";
        return cmd_commit(msg, false);
    }
    if (cmd == "history") {
        int limit = 0;
        if (argc >= 3) limit = std::atoi(args[2].c_str());
        return cmd_history(limit);
    }
    if (cmd == "show") {
        uint32_t cid = 0;
        if (argc < 3 || !parse_uint32_arg(args[2], cid)) {
            std::fprintf(stderr, "Usage: fvc show <commit_id>\n");
            return 1;
        }
        return cmd_show(cid);
    }
    if (cmd == "revert") {
        uint32_t cid = INVALID_COMMIT;
        if (argc >= 3 && !parse_uint32_arg(args[2], cid)) {
            std::fprintf(stderr, "Usage: fvc revert [commit_id]\n");
            return 1;
        }
        return cmd_revert(cid);
    }
    if (cmd == "date" || cmd == "log") {
        std::string date_str = (argc >= 3) ? args.back() : "";
        uint32_t ymd = 0;
        if (!parse_ddmmyyyy_to_ymd(date_str, ymd)) {
            std::fprintf(stderr, "Usage: fvc date <DDMMYYYY> (e.g., fvc date 18112026)\n");
            return 1;
        }
        return cmd_date_query(ymd);
    }
    if (cmd == "purge") {
        std::string date_str = (argc >= 3) ? args.back() : "";
        uint32_t ymd = 0;
        if (!parse_ddmmyyyy_to_ymd(date_str, ymd)) {
            std::fprintf(stderr, "Usage: fvc purge <DDMMYYYY> (e.g., fvc purge 18112026)\n");
            return 1;
        }
        return cmd_purge_date(ymd);
    }

    print_usage();
    return 1;
}