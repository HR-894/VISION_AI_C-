/**
 * @file file_manager.cpp
 * @brief Filesystem operations with SafetyGuard integration
 */

#include "file_manager.h"
#include "safety_guard.h"
#include <algorithm>
#include <sstream>
#include <filesystem>
#include <iomanip>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_WARN(...) (void)0
#define LOG_ERROR(...) (void)0
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace vision {

namespace {

std::wstring utf8ToWidePath(const std::string& text) {
    if (text.empty()) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return {};

    std::wstring wide(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), wlen);
    if (!wide.empty() && wide.back() == L'\0') {
        wide.pop_back();
    }
    return wide;
}

} // namespace

FileManager::FileManager(SafetyGuard& guard)
    : guard_(guard) {}

// ═══════════════════ Listing ═══════════════════

std::vector<FileInfo> FileManager::listFiles(const std::string& path,
                                              const std::string& filter) {
    std::vector<FileInfo> result;
    std::string dir_path = path;
    
    if (dir_path.empty()) {
        char prof[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, prof))) {
            dir_path = prof;
        } else {
            dir_path = ".";
        }
    }
    
    fs::path dir(dir_path);
    if (!fs::exists(dir) || !fs::is_directory(dir)) return result;
    
    std::string lower_filter = filter;
    std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(), ::tolower);
    
    for (const auto& entry : fs::directory_iterator(dir)) {
        FileInfo info;
        info.name = entry.path().filename().string();
        info.full_path = entry.path().string();
        info.is_directory = entry.is_directory();
        info.extension = entry.path().extension().string();
        
        if (!info.is_directory) {
            try { info.size_bytes = entry.file_size(); }
            catch (...) { info.size_bytes = 0; }
        }
        
        // Apply filter
        if (!lower_filter.empty() && lower_filter != "*" && lower_filter != "all") {
            std::string lower_name = info.name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
            
            if (lower_name.find(lower_filter) == std::string::npos) {
                continue;
            }
        }
        
        result.push_back(info);
    }
    
    // Sort: directories first, then by name
    std::sort(result.begin(), result.end(), [](const FileInfo& a, const FileInfo& b) {
        if (a.is_directory != b.is_directory) return a.is_directory > b.is_directory;
        return a.name < b.name;
    });
    
    return result;
}

std::vector<FileInfo> FileManager::listDownloads() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, path))) {
        return listFiles(std::string(path) + "\\Downloads");
    }
    return {};
}

std::vector<FileInfo> FileManager::listDesktop() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, path))) {
        return listFiles(std::string(path));
    }
    return {};
}

std::vector<FileInfo> FileManager::listDocuments() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, path))) {
        return listFiles(std::string(path));
    }
    return {};
}

// ═══════════════════ File Operations ═══════════════════

std::pair<bool, std::string> FileManager::copyFile(const std::string& src,
                                                     const std::string& dst) {
    auto [src_ok, src_msg] = guard_.validateFileOperation("copy", src);
    if (!src_ok) return {false, "Copy blocked for source: " + src_msg};

    auto [dst_ok, dst_msg] = guard_.validateFileOperation("copy", dst);
    if (!dst_ok) return {false, "Copy blocked for destination: " + dst_msg};
    
    try {
        fs::path src_path(src);
        fs::path dst_path(dst);

        if (!fs::exists(src_path)) {
            return {false, "Source does not exist: " + src};
        }
        
        if (fs::is_directory(src_path)) {
            fs::copy(src_path, dst_path,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        } else {
            if (dst_path.has_parent_path()) {
                fs::create_directories(dst_path.parent_path());
            }
            fs::copy_file(src_path, dst_path, fs::copy_options::overwrite_existing);
        }
        
        guard_.logAction("copy", src + " -> " + dst, "success");
        return {true, "Copied: " + src_path.filename().string()};
    } catch (const std::exception& e) {
        return {false, "Copy failed: " + std::string(e.what())};
    }
}

std::pair<bool, std::string> FileManager::moveFile(const std::string& src,
                                                     const std::string& dst) {
    auto [src_ok, src_msg] = guard_.validateFileOperation("move", src);
    if (!src_ok) return {false, "Move blocked for source: " + src_msg};

    auto [dst_ok, dst_msg] = guard_.validateFileOperation("move", dst);
    if (!dst_ok) return {false, "Move blocked for destination: " + dst_msg};
    
    try {
        fs::path src_path(src);
        fs::path dst_path(dst);

        if (!fs::exists(src_path)) {
            return {false, "Source does not exist: " + src};
        }
        if (dst_path.has_parent_path()) {
            fs::create_directories(dst_path.parent_path());
        }
        fs::rename(src_path, dst_path);
        guard_.logAction("move", src + " -> " + dst, "success");
        return {true, "Moved: " + src_path.filename().string()};
    } catch (const std::exception& e) {
        return {false, "Move failed: " + std::string(e.what())};
    }
}

std::pair<bool, std::string> FileManager::renameFile(const std::string& path,
                                                       const std::string& new_name) {
    auto [ok, msg] = guard_.validateFileOperation("rename", path);
    if (!ok) return {false, msg};
    
    try {
        fs::path old_p(path);
        fs::path requested_name(new_name);

        if (new_name.empty() || requested_name.is_absolute() || requested_name.has_parent_path()) {
            return {false, "Rename target must be a plain file or folder name"};
        }

        fs::path new_p = old_p.parent_path() / requested_name.filename();
        auto [dst_ok, dst_msg] = guard_.validateFileOperation("rename", new_p.string());
        if (!dst_ok) return {false, "Rename blocked for destination: " + dst_msg};

        fs::rename(old_p, new_p);
        guard_.logAction("rename", path + " -> " + new_name, "success");
        return {true, "Renamed to: " + new_name};
    } catch (const std::exception& e) {
        return {false, "Rename failed: " + std::string(e.what())};
    }
}

std::pair<bool, std::string> FileManager::deleteToRecycleBin(const std::string& path) {
    auto [ok, msg] = guard_.validateFileOperation("delete_recycle", path);
    if (!ok) return {false, msg};
    
    std::wstring wpath = utf8ToWidePath(path);
    if (wpath.empty()) {
        return {false, "Failed to encode path for recycle bin operation"};
    }

    std::vector<wchar_t> from_buffer(wpath.begin(), wpath.end());
    from_buffer.push_back(L'\0');
    from_buffer.push_back(L'\0');
    
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from_buffer.data();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    
    int result = SHFileOperationW(&op);
    if (result == 0 && !op.fAnyOperationsAborted) {
        guard_.logAction("delete_recycle", path, "success");
        return {true, "Moved to recycle bin: " + fs::path(path).filename().string()};
    }
    return {false, "Failed to delete: " + path};
}

// ═══════════════════ Search ═══════════════════

std::vector<FileInfo> FileManager::searchFiles(const std::string& directory,
                                                 const std::string& query,
                                                 bool recursive, int max_results) {
    std::vector<FileInfo> results;
    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);
    
    try {
        fs::path dir(directory);
        if (!fs::exists(dir)) return results;
        
        auto process_entry = [&](const fs::directory_entry& entry) {
            if (static_cast<int>(results.size()) >= max_results) return;
            
            std::string name = entry.path().filename().string();
            std::string lower_name = name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
            
            if (lower_name.find(lower_query) != std::string::npos) {
                FileInfo info;
                info.name = name;
                info.full_path = entry.path().string();
                info.is_directory = entry.is_directory();
                info.extension = entry.path().extension().string();
                if (!info.is_directory) {
                    try { info.size_bytes = entry.file_size(); } catch (...) {}
                }
                results.push_back(info);
            }
        };
        
        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(
                     dir, fs::directory_options::skip_permission_denied)) {
                process_entry(entry);
                if (static_cast<int>(results.size()) >= max_results) break;
            }
        } else {
            for (const auto& entry : fs::directory_iterator(dir)) {
                process_entry(entry);
                if (static_cast<int>(results.size()) >= max_results) break;
            }
        }
    } catch (...) {
        LOG_WARN("Search error in: {}", directory);
    }
    
    return results;
}

nlohmann::json FileManager::organizeByType(const std::string& path) {
    json organized;
    auto files = listFiles(path);
    
    for (const auto& f : files) {
        if (f.is_directory) continue;
        std::string ext = f.extension;
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        std::string category = getFileCategory(ext);
        organized[category].push_back(f.name);
    }
    
    return organized;
}

std::string FileManager::resolveLocation(const std::string& location) {
    std::string lower = location;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    char path[MAX_PATH];
    if (lower == "downloads") {
        SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, path);
        return std::string(path) + "\\Downloads";
    }
    if (lower == "desktop") {
        SHGetFolderPathA(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, path);
        return path;
    }
    if (lower == "documents") {
        SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, path);
        return path;
    }
    if (lower == "pictures" || lower == "photos") {
        SHGetFolderPathA(nullptr, CSIDL_MYPICTURES, nullptr, 0, path);
        return path;
    }
    if (lower == "music") {
        SHGetFolderPathA(nullptr, CSIDL_MYMUSIC, nullptr, 0, path);
        return path;
    }
    if (lower == "videos") {
        SHGetFolderPathA(nullptr, CSIDL_MYVIDEO, nullptr, 0, path);
        return path;
    }
    return location;
}

std::string FileManager::formatFileSize(std::uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024 && unit < 4) { size /= 1024; unit++; }
    std::ostringstream ss;
    ss << std::fixed;
    ss.precision(unit > 0 ? 1 : 0);
    ss << size << " " << units[unit];
    return ss.str();
}

std::string FileManager::getFileExtension(const std::string& filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) return "";
    return filename.substr(dot);
}

std::string FileManager::getFileCategory(const std::string& ext) {
    std::string e = ext;
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    
    if (e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".gif" || e == ".bmp" || e == ".webp" || e == ".svg")
        return "Images";
    if (e == ".mp4" || e == ".avi" || e == ".mkv" || e == ".mov" || e == ".wmv" || e == ".flv")
        return "Videos";
    if (e == ".mp3" || e == ".wav" || e == ".flac" || e == ".aac" || e == ".ogg" || e == ".wma")
        return "Audio";
    if (e == ".pdf" || e == ".doc" || e == ".docx" || e == ".txt" || e == ".xlsx" || e == ".pptx" || e == ".csv")
        return "Documents";
    if (e == ".zip" || e == ".rar" || e == ".7z" || e == ".tar" || e == ".gz")
        return "Archives";
    if (e == ".exe" || e == ".msi" || e == ".bat" || e == ".cmd")
        return "Programs";
    if (e == ".py" || e == ".cpp" || e == ".h" || e == ".js" || e == ".ts" || e == ".html" || e == ".css")
        return "Code";
    return "Other";
}

} // namespace vision
