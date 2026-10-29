// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/type_shard_database_gen.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardDatabaseType : private ShardDatabaseTypeBase {
public:
    // Make field names accessible.
    using ShardDatabaseTypeBase::kDbNameFieldName;
    using ShardDatabaseTypeBase::kEnterCriticalSectionCounterFieldName;

    // Make getters and setters accessible.
    using ShardDatabaseTypeBase::getDbName;
    using ShardDatabaseTypeBase::getPrimary;
    using ShardDatabaseTypeBase::getVersion;

    explicit ShardDatabaseType(const BSONObj& obj);
};

}  // namespace mongo
