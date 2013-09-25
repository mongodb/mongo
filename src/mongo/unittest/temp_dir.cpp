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
 */

#include "mongo/platform/basic.h"

#include "mongo/unittest/temp_dir.h"

#include <boost/filesystem.hpp>

#include "mongo/base/init.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/log.h"


namespace mongo {
namespace unittest {
    namespace str = mongoutils::str;

namespace {
    boost::filesystem::path defaultRoot;

    MONGO_INITIALIZER(SetTempDirDefaultRoot)(InitializerContext* context) {
        // TODO add a --tempPath parameter and use that if specified.
        defaultRoot = boost::filesystem::temp_directory_path();

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
        }
        catch (const std::exception& e) {
            warning() << "error encountered recursively deleting directory '" << _path << "': "
                      << e.what() << ". Ignoring and continuing.";
        }
    }
} // namespace unittest
} // namespace mongo
