#pragma once
/**
 * @file file_manager.h
 * @brief Filesystem operations with safety guard integration
 * 
 * List, copy, move, rename, delete (to recycle bin), search files.
 * All destructive operations go through SafetyGuard validation.
 */

#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace vision {

class SafetyGuard; // Forward declaration

/// Information about a file or directory
struct FileInfo {
    std::string name;
    std::string full_path;
    std::string extension;
    std::uintmax_t size_bytes = 0;
    bool is_directory = false;
    std::string modified_time;
};

class FileManager {
public:
    explicit FileManager(SafetyGuard& guard);

    /// List files in a directory. Empty dir = user home.
    std::vector<FileInfo> listFiles(const std::string& dir = "",
                                     const std::string& pattern = "*");

    /// List well-known folders
    std::vector<FileInfo> listDownloads();
    std::vector<FileInfo> listDesktop();
    std::vector<FileInfo> listDocuments();

    /// Copy file or directory
    std::pair<bool, std::string> copyFile(const std::string& src,
                                           const std::string& dst);

    /// Move file or directory
    std::pair<bool, std::string> moveFile(const std::string& src,
                                           const std::string& dst);

    /// Rename file or directory
    std::pair<bool, std::string> renameFile(const std::string& old_path,
                                             const std::string& new_name);

    /// Delete to recycle bin (SHFileOperation with FOF_ALLOWUNDO)
    std::pair<bool, std::string> deleteToRecycleBin(const std::string& path);

    /// Search files recursively
    std::vector<FileInfo> searchFiles(const std::string& dir,
                                       const std::string& pattern,
                                       bool recursive = true,
                                       int max_results = 50);

    /// Organize files in a directory by type
    nlohmann::json organizeByType(const std::string& dir);

    /// Resolve well-known folder names ("downloads", "desktop", etc.)
    std::string resolveLocation(const std::string& location);

    /// Format file size to human-readable string
    static std::string formatFileSize(std::uintmax_t bytes);

private:
    SafetyGuard& guard_;

    std::string getFileExtension(const std::string& filename);
    std::string getFileCategory(const std::string& extension);
};

} // namespace vision
