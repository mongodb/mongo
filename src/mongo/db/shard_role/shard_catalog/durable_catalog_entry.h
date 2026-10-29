// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/durable_catalog_entry_metadata.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {
namespace durable_catalog {
/**
 * Parsed catalog entry of a single `_mdb_catalog` document.
 */
struct [[MONGO_MOD_NEEDS_REPLACEMENT]] CatalogEntry {
    RecordId catalogId;
    std::string ident;
    const BSONObj indexIdents;
    std::shared_ptr<durable_catalog::CatalogEntryMetaData> metadata;
};

}  // namespace durable_catalog
}  // namespace mongo
