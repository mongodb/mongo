/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/unittest/temp_dir.h"

#include <boost/filesystem.hpp>

#include "mongo/base/init.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"


namespace mongo {

using std::string;

namespace unittest {
namespace str = mongoutils::str;
namespace moe = mongo::optionenvironment;

namespace {
boost::filesystem::path defaultRoot;

MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(TempDirOptions)(InitializerContext* context) {
    moe::startupOptions.addOptionChaining(
        "tempPath", "tempPath", moe::String, "directory to place mongo::TempDir subdirectories");
    return Status::OK();
}

MONGO_INITIALIZER(SetTempDirDefaultRoot)(InitializerContext* context) {
    if (moe::startupOptionsParsed.count("tempPath")) {
        defaultRoot = moe::startupOptionsParsed["tempPath"].as<string>();
    } else {
        defaultRoot = boost::filesystem::temp_directory_path();
    }

    if (!boost::filesystem::exists(defaultRoot)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Attempted to use a tempPath (" << defaultRoot.string()
                                    << ") that doesn't exist");
    }

    if (!boost::filesystem::is_directory(defaultRoot)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Attempted to use a tempPath (" << defaultRoot.string()
                                    << ") that exists, but isn't a directory");
    }
    return Status::OK();
}
}

TempDir::TempDir(const std::string& namePrefix) {
    fassert(17146, namePrefix.find_first_of("/\\") == std::string::npos);

    // This gives the dir name 64 bits of randomness.
    const boost::filesystem::path dirName =
        boost::filesystem::unique_path(namePrefix + "-%%%%-%%%%-%%%%-%%%%");

    _path = (defaultRoot / dirName).string();

    bool createdNewDirectory = boost::filesystem::create_directory(_path);
    if (!createdNewDirectory) {
        error() << "unique path (" << _path << ") already existed";
        fassertFailed(17147);
    }

    ::mongo::unittest::log() << "Created temporary directory: " << _path;
}

TempDir::~TempDir() {
    try {
        boost::filesystem::remove_all(_path);
    } catch (const std::exception& e) {
        warning() << "error encountered recursively deleting directory '" << _path
                  << "': " << e.what() << ". Ignoring and continuing.";
    }
}
}  // namespace unittest
}  // namespace mongo
