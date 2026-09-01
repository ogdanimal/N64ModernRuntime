#ifndef __RECOMP_FILES_H__
#define __RECOMP_FILES_H__

#include <filesystem>
#include <fstream>
#include <string>

namespace recomp {
    std::ifstream open_input_file_with_backup(const std::filesystem::path& filepath, std::ios_base::openmode mode = std::ios_base::in);
    std::ifstream open_input_backup_file(const std::filesystem::path& filepath, std::ios_base::openmode mode = std::ios_base::in);

    // The `error_detail` out-params are optional and only written on failure.
    // They carry a short human-readable reason: which step failed and what the
    // OS said about it. Callers that surface a failure to the user should pass
    // one -- these functions have several distinct failure modes that are
    // indistinguishable from the outside, and a report of "saving failed" with
    // no reason costs a round trip with whoever hit it.
    std::ofstream open_output_file_with_backup(const std::filesystem::path& filepath, std::ios_base::openmode mode = std::ios_base::out, std::string* error_detail = nullptr);
    bool finalize_output_file_with_backup(const std::filesystem::path& filepath, std::string* error_detail = nullptr);
};

#endif
