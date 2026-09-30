// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace mongo {
/**
 * Native shell callbacks used by JavaScript tests to write and read named pipes.
 *
 * Exceptions occurring on the calling thread will be propagated back to the JavaScript shell as
 * JavaScript exceptions. Exceptions occurring asynchronously on detached threads will be logged.
 */
class NamedPipeHelper {
public:
    static BSONObj readFromPipes(const std::vector<std::string>& pipeRelativePaths);
    static void writeToPipeAsync(std::string pipeDir,
                                 std::string pipeRelativePath,
                                 long objects,
                                 long stringMinSize,
                                 long stringMaxSize);
    static void writeToPipeObjects(std::string pipeDir,
                                   std::string pipeRelativePath,
                                   long objects,
                                   std::vector<BSONObj> bsonObjs,
                                   bool persistPipe = false);
    static void writeToPipeObjectsAsync(std::string pipeDir,
                                        std::string pipeRelativePath,
                                        long objects,
                                        std::vector<BSONObj> bsonObjs,
                                        bool persistPipe = false);
};
}  // namespace mongo
