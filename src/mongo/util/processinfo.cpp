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


#include <fstream>  // IWYU pragma: keep
#include <system_error>

#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/logv2/log.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/processinfo.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

namespace {
MONGO_INITIALIZER(initApplicationInfo)(InitializerContext* context) {
    ProcessInfo().appInfo().init(context->args());
}
}  // namespace

class PidFileWiper {
public:
    ~PidFileWiper() {
        if (path.empty()) {
            return;
        }

        std::ofstream out(path.c_str(), std::ios_base::out);
        out.close();
    }

    bool write(const boost::filesystem::path& p) {
        path = p;
        std::ofstream out(path.c_str(), std::ios_base::out);
        out << ProcessId::getCurrent() << std::endl;
        if (!out.good()) {
            auto ec = lastSystemError();
            if (!ec) {
                LOGV2(23329,
                      "ERROR: Cannot write pid file to {path_string}: Unable to determine OS error",
                      "path_string"_attr = path.string());
            } else {
                LOGV2(23330,
                      "ERROR: Cannot write pid file to {path_string}: {errAndStr_second}",
                      "path_string"_attr = path.string(),
                      "errAndStr_second"_attr = errorMessage(ec));
            }
        } else {
            boost::system::error_code ec;
            boost::filesystem::permissions(
                path,
                boost::filesystem::owner_read | boost::filesystem::owner_write |
                    boost::filesystem::group_read | boost::filesystem::others_read,
                ec);
            if (ec) {
                LOGV2(23331,
                      "Could not set permissions on pid file {path_string}: {ec_message}",
                      "path_string"_attr = path.string(),
                      "ec_message"_attr = ec.message());
                return false;
            }
        }
        return out.good();
    }

private:
    boost::filesystem::path path;
} pidFileWiper;

bool writePidFile(const std::string& path) {
    return pidFileWiper.write(path);
}
}  // namespace mongo
