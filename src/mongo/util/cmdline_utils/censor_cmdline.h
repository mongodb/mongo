// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

[[MONGO_MOD_PUBLIC]];

namespace mongo::cmdline_utils {

/**
 * Blot out sensitive fields in the argv array.
 */
void censorArgvArray(int argc, char** argv);
void censorArgsVector(std::vector<std::string>* args);
void censorBSONObj(BSONObj* params);

}  // namespace mongo::cmdline_utils
