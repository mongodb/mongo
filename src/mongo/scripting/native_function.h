// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * A callback invokable by the scripting engine.
 *
 * This must remain engine-agnostic (usable by mozjs, wasm, etc).
 */
using NativeFunction [[MONGO_MOD_PUBLIC]] = BSONObj (*)(const BSONObj& args, void* data);

}  // namespace mongo
