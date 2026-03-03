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

#include "mongo/scripting/mongo_path_util.h"

#include <cstdlib>
#include <cstring>

#include <boost/filesystem.hpp>

namespace mongo {

std::vector<std::string> parseMongoPath() {
    std::vector<std::string> paths;

    const char* mongoPath = std::getenv("MONGO_PATH");
    if (!mongoPath || std::strlen(mongoPath) == 0) {
        // Default to current working directory if MONGO_PATH not set
        paths.push_back(boost::filesystem::current_path().string());
        return paths;
    }

#ifdef _WIN32
    const char delimiter = ';';
#else
    const char delimiter = ':';
#endif

    std::string pathStr(mongoPath);
    size_t start = 0;
    size_t end = pathStr.find(delimiter);

    while (end != std::string::npos) {
        std::string path = pathStr.substr(start, end - start);
        if (!path.empty()) {
            paths.push_back(path);
        }
        start = end + 1;
        end = pathStr.find(delimiter, start);
    }

    // Add the last path (or only path if no delimiter found)
    std::string lastPath = pathStr.substr(start);
    if (!lastPath.empty()) {
        paths.push_back(lastPath);
    }

    return paths;
}

}  // namespace mongo
