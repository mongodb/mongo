/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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

stdx::mutex tempPathRootMutex;
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
