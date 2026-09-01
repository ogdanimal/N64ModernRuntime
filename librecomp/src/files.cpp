#include <cerrno>

#include "files.hpp"

constexpr std::u8string_view backup_suffix = u8".bak";
constexpr std::u8string_view temp_suffix = u8".temp";

static void set_detail(std::string* error_detail, std::string detail) {
    if (error_detail != nullptr) {
        *error_detail = std::move(detail);
    }
}

// Move `from` onto `to`, replacing whatever is already there.
//
// A rename rather than a copy, because the two need DIFFERENT permissions.
// Copying onto an existing file has to open that file for writing, so it fails
// whenever the destination is not writable by us even though its directory is.
// A rename only needs write permission on the containing directory; it replaces
// the directory entry rather than writing through the old inode.
//
// That difference is not theoretical. A user who sideloads a save file into the
// app's data dir (`adb push`, a file manager, a restore tool) can end up with a
// save file the app itself cannot write through. With a copy, every subsequent
// save fails and stays failing, because each attempt hits the same wall. With a
// rename the write succeeds, the file is ours again afterwards, and the
// situation heals itself after one save.
//
// It is also atomic, which a copy is not: a copy interrupted partway leaves a
// truncated file behind. For the `.bak` slot that is the worse outcome of the
// two, since a rollback point that is only sometimes intact is one you cannot
// reason about.
//
// Falls back to a copy when the rename fails, purely as a conservative safety
// net: rename is the stricter primitive of the two (same filesystem only, and
// Android's FUSE-backed emulated storage has historically been uneven about
// it), so on any volume that refuses it we still behave exactly as before
// rather than regressing into a save that cannot be written at all. No
// currently-known case reaches the fallback -- a directory we cannot rename
// inside is also one we could not have created the `.temp` file in, so that
// failure lands earlier -- and it is kept for the unknown ones.
static std::string replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    // One case where the old copy semantics are the RIGHT ones and must be
    // kept: a destination that is a symlink. A copy follows the link and writes
    // through to its target, while a rename would quietly replace the link
    // itself with a regular file. Someone who has pointed their save file at a
    // synced folder that way would find their setup silently detached, and
    // would have no reason to suspect this. Rare, and impossible inside an
    // Android app's own data dir, but the desktop builds share this code.
    std::error_code symlink_ec;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(to, symlink_ec))) {
        std::error_code copy_ec;
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            return "could not write through the symlink at \"" + to.string() + "\": " + copy_ec.message();
        }
        std::error_code remove_ec;
        std::filesystem::remove(from, remove_ec);
        return {};
    }

    std::error_code rename_ec;
    std::filesystem::rename(from, to, rename_ec);
    if (!rename_ec) {
        return {};
    }

    std::error_code copy_ec;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, copy_ec);
    if (copy_ec) {
        return "rename failed (" + rename_ec.message() + ") and so did the copy fallback (" + copy_ec.message() + ")";
    }

    std::error_code remove_ec;
    std::filesystem::remove(from, remove_ec);
    return {};
}

std::ifstream recomp::open_input_backup_file(const std::filesystem::path& filepath, std::ios_base::openmode mode) {
    std::filesystem::path backup_path{filepath};
    backup_path += backup_suffix;
    return std::ifstream{backup_path, mode};
}

std::ifstream recomp::open_input_file_with_backup(const std::filesystem::path& filepath, std::ios_base::openmode mode) {
    std::ifstream ret{filepath, mode};

    // Check if the file failed to open and open the corresponding backup file instead if so.
    if (!ret.good()) {
        return open_input_backup_file(filepath, mode);
    }

    return ret;
}

std::ofstream recomp::open_output_file_with_backup(const std::filesystem::path& filepath, std::ios_base::openmode mode, std::string* error_detail) {
    std::filesystem::path temp_path{filepath};
    temp_path += temp_suffix;

    errno = 0;
    std::ofstream temp_file_out{ temp_path, mode };

    if (!temp_file_out.good() && error_detail != nullptr) {
        // errno is read immediately after the failed open, before anything else
        // can clobber it. It is the only account of WHY we could not create the
        // file, and the stream itself does not carry one.
        std::string reason = errno != 0
            ? std::error_code{ errno, std::generic_category() }.message()
            : std::string{"unknown error"};

        // The overwhelmingly common cause of this in the wild is the containing
        // directory not being a directory at all: sideloading tools happily
        // create a plain FILE where the folder was supposed to go (`adb push
        // save.bin .../saves` does exactly that when `saves` does not exist
        // yet), and from then on nothing can be created inside it. Name that
        // case outright, because "Not a directory" on its own does not tell
        // anyone which path is the problem or what to do about it.
        std::error_code ec;
        std::filesystem::path parent = temp_path.parent_path();
        if (std::filesystem::exists(parent, ec) && !std::filesystem::is_directory(parent, ec)) {
            reason = "a file is in the way of the folder \"" + parent.string() +
                     "\" -- delete it and the folder will be recreated";
        }

        set_detail(error_detail, "could not create \"" + temp_path.string() + "\": " + reason);
    }

    return temp_file_out;
}

bool recomp::finalize_output_file_with_backup(const std::filesystem::path& filepath, std::string* error_detail) {
    std::filesystem::path backup_path{filepath};
    backup_path += backup_suffix;

    std::filesystem::path temp_path{filepath};
    temp_path += temp_suffix;

    std::error_code ec;
    if (std::filesystem::exists(filepath, ec)) {
        // Stage the backup beside its final name and move it into place, so the
        // `.bak` slot is never observable half-written.
        std::filesystem::path backup_temp_path{backup_path};
        backup_temp_path += temp_suffix;

        std::filesystem::copy_file(filepath, backup_temp_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            set_detail(error_detail, "could not stage the backup \"" + backup_temp_path.string() + "\": " + ec.message());
            return false;
        }

        std::string err = replace_file(backup_temp_path, backup_path);
        if (!err.empty()) {
            set_detail(error_detail, "could not update the backup \"" + backup_path.string() + "\": " + err);
            std::error_code remove_ec;
            std::filesystem::remove(backup_temp_path, remove_ec);
            return false;
        }
    }

    std::string err = replace_file(temp_path, filepath);
    if (!err.empty()) {
        set_detail(error_detail, "could not move \"" + temp_path.string() + "\" into place: " + err);
        return false;
    }

    std::filesystem::remove(temp_path, ec);
    return true;
}
