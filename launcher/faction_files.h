#pragma once

#include <string>
#include <optional>
#include <functional>
#include <chrono>
#include <common/HttpRequest.h>
#include <launcher_common/PatchedAppLauncher.h>
#include <unzip.h>

class FactionFilesAFLink
{
public:
    struct FileInfo
    {
        std::string name;
        std::string author;
        std::string description;
        unsigned size_in_bytes;
        int file_id;
        std::string file_type;
    };

    FactionFilesAFLink();
    std::optional<FileInfo> get_file_info(int file_id);
    bool download_and_extract(int file_id, std::function<bool(
        unsigned bytes_received, std::chrono::milliseconds duration)> progress_callback);

private:
    HttpSession session_;

    static std::optional<FileInfo> parse_file_info(const std::string& response);
    static std::string get_extraction_path(const std::string& game_exe_path, const std::string& file_type);
    static bool extract_zip(const std::string& zip_path, const std::string& extract_to);
};
