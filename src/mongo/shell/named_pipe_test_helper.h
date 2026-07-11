// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace mongo {
/**
 * This class supports writing and reading named pipes in the mongo test shell.
 */
class NamedPipeHelper {
public:
    static BSONObj readFromPipes(const std::vector<std::string>& pipeRelativePaths);
    static void writeToPipe(std::string pipeDir,
                            std::string pipeRelativePath,
                            long objects,
                            long stringMinSize,
                            long stringMaxSize) noexcept;
    static void writeToPipeAsync(std::string pipeDir,
                                 std::string pipeRelativePath,
                                 long objects,
                                 long stringMinSize,
                                 long stringMaxSize);
    static void writeToPipeObjects(std::string pipeDir,
                                   std::string pipeRelativePath,
                                   long objects,
                                   std::vector<BSONObj> bsonObjs,
                                   bool persistPipe = false) noexcept;
    static void writeToPipeObjectsAsync(std::string pipeDir,
                                        std::string pipeRelativePath,
                                        long objects,
                                        std::vector<BSONObj> bsonObjs,
                                        bool persistPipe = false);
};
}  // namespace mongo
