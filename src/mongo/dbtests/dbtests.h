// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo::dbtests {

/**
 * Combines AutoGetDb and AutoStatsTracker. If the requested 'ns' exists, the constructed
 * object will have both the database and the collection locked in MODE_IX. Otherwise, the database
 * will be locked in MODE_IX and will be created, while the collection will be locked in MODE_X, but
 * not created.
 */
class WriteContextForTests {
    WriteContextForTests(const WriteContextForTests&) = delete;
    WriteContextForTests& operator=(const WriteContextForTests&) = delete;

public:
    WriteContextForTests(OperationContext* opCtx, std::string_view ns);

    Database* db() const {
        return _autoDb->getDb();
    }

    CollectionAcquisition getOrCreateCollection(LockMode mode = MODE_IX);
    CollectionAcquisition getCollection(LockMode mode = MODE_IX) const;

private:
    OperationContext* const _opCtx;
    const NamespaceString _nss;

    boost::optional<AutoGetDb> _autoDb;
    boost::optional<AutoStatsTracker> _tracker;
};

}  // namespace mongo::dbtests
