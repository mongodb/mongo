// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"

namespace mongo {

BSONObj native_hex_md5(const BSONObj& args, void* data);

}  // namespace mongo
