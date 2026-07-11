// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/util/modules.h"

namespace mongo {

class Scope;

namespace shell_utils {
void installShellUtilsExtended(Scope& scope);
BSONObj ls(const BSONObj& args, void* data);
BSONObj removeFile(const BSONObj& args, void* data);
BSONObj getObjInDumpFile(const BSONObj& a, void*);
BSONObj numObjsInDumpFile(const BSONObj& a, void*);
BSONObj readDumpFile(const BSONObj& a, void*);
BSONObj writeBsonArrayToFile(const BSONObj& args, void* data);

}  // namespace shell_utils
}  // namespace mongo
