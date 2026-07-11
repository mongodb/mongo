// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mongo_path_util.h"

#include <cstdlib>
#include <cstring>

#include <boost/filesystem.hpp>

namespace mongo {

std::vector<std::string> parseMongoPath() {
    return parseMongoPath(boost::filesystem::current_path().string());
}

std::vector<std::string> parseMongoPath(const std::string& defaultPath) {
    std::vector<std::string> paths;

    const char* mongoPath = std::getenv("MONGO_PATH");
    if (!mongoPath || std::strlen(mongoPath) == 0) {
        // Use the provided default path if MONGO_PATH not set
        paths.push_back(defaultPath);
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
