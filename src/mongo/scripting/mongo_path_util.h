// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace mongo {

/**
 * Parse MONGO_PATH environment variable into a list of search directories.
 *
 * MONGO_PATH is a path list similar to the system PATH variable, used to locate
 * JavaScript files for both load() and import() operations.
 *
 * Format:
 * - Unix/Linux/macOS: colon-separated paths (/path1:/path2:/path3)
 * - Windows: semicolon-separated paths (C:\path1;C:\path2;C:\path3)
 *
 * Behavior:
 * - If MONGO_PATH is set: returns the explicit paths from MONGO_PATH
 * - If MONGO_PATH is not set: returns the defaultPath (or cwd if no default provided)
 * - Empty path components are skipped
 * - Paths are returned in the order they appear in MONGO_PATH
 *
 * Example:
 *   MONGO_PATH=/usr/local/lib/mongo:/opt/mongo/lib
 *   Returns: ["/usr/local/lib/mongo", "/opt/mongo/lib"]
 *
 * @param defaultPath The path to use when MONGO_PATH is not set
 * @return Vector of directory paths to search, in priority order
 */
std::vector<std::string> parseMongoPath(const std::string& defaultPath);

/**
 * Parse MONGO_PATH environment variable with current working directory as default.
 * Convenience overload that uses cwd when MONGO_PATH is not set.
 *
 * @return Vector of directory paths to search, in priority order
 */
std::vector<std::string> parseMongoPath();

}  // namespace mongo
