// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/unittest/temp_dir.h"

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/options_parser/value.h"
#include "mongo/util/str.h"

#include <exception>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace unittest {
namespace {

std::mutex tempPathRootMutex;
boost::filesystem::path tempPathRoot;

void setTempPathRoot(boost::filesystem::path root) {
    std::lock_guard lg(tempPathRootMutex);

    if (!boost::filesystem::exists(root)) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Attempted to use a tempPath (" << root.string()
                                << ") that doesn't exist");
    }

    if (!boost::filesystem::is_directory(root)) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Attempted to use a tempPath (" << root.string()
                                << ") that exists, but isn't a directory");
    }
    tempPathRoot = std::move(root);
}

const boost::filesystem::path& getTempPathRoot() {
    std::lock_guard lg(tempPathRootMutex);

    if (tempPathRoot.empty()) {
        tempPathRoot = boost::filesystem::temp_directory_path();
    }

    uassert(8448300, "Unable to set temp directory", !tempPathRoot.empty());
    return tempPathRoot;
}

}  // namespace

TempDir::TempDir(const std::string& namePrefix) {
    fassert(17146, namePrefix.find_first_of("/\\") == std::string::npos);

    // This gives the dir name 64 bits of randomness.
    const boost::filesystem::path dirName =
        boost::filesystem::unique_path(namePrefix + "-%%%%-%%%%-%%%%-%%%%");

    _path = (getTempPathRoot() / dirName).string();

    bool createdNewDirectory = boost::filesystem::create_directory(_path);
    if (!createdNewDirectory) {
        LOGV2_ERROR(23053, "unique path ({path}) already existed", "path"_attr = _path);
        fassertFailed(17147);
    }

    LOGV2_DEBUG(23051, 1, "Created temporary directory: {path}", "path"_attr = _path);
}

TempDir::~TempDir() {
    if (_path.empty())
        return;

    try {
        boost::filesystem::remove_all(_path);
    } catch (const std::exception& e) {
        LOGV2_WARNING(23052,
                      "error encountered recursively deleting directory '{path}': {e_what}. "
                      "Ignoring and continuing.",
                      "path"_attr = _path,
                      "e_what"_attr = e.what());
    }
}

void TempDir::setTempPath(std::string tempPath) {
    setTempPathRoot(std::move(tempPath));
}

}  // namespace unittest
}  // namespace mongo
