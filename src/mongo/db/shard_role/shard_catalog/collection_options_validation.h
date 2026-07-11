// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

namespace mongo::collection_options_validation {

[[MONGO_MOD_PRIVATE]]
Status validateStorageEngineOptions(const BSONObj& storageEngine);

}  // namespace mongo::collection_options_validation
