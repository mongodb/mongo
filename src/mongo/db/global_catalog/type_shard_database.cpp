// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_shard_database.h"

#include "mongo/idl/idl_parser.h"

namespace mongo {

ShardDatabaseType::ShardDatabaseType(const BSONObj& obj) {
    ShardDatabaseTypeBase::parseProtected(obj, IDLParserContext("ShardDatabaseTypeBase"));
}

}  // namespace mongo
