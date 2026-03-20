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
 * - If MONGO_PATH is not set or empty, returns the current working directory
 * - Empty path components are skipped
 * - Paths are returned in the order they appear in MONGO_PATH
 *
 * Example:
 *   MONGO_PATH=/usr/local/lib/mongo:/opt/mongo/lib
 *   Returns: ["/usr/local/lib/mongo", "/opt/mongo/lib"]
 *
 * @return Vector of directory paths to search, in priority order
 */
std::vector<std::string> parseMongoPath();

}  // namespace mongo
