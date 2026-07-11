// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace repl {

CollectionOptions parseCollectionOptionsForDatabaseCloner(const BSONObj& obj);
}
}  // namespace mongo
