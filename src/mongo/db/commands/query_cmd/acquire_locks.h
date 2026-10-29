// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/shard_role/shard_catalog/external_data_source_scope_guard.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional.hpp>

namespace mongo {

void applyConcernsAndReadPreference(OperationContext* opCtx, const ClientCursor& cursor);

struct CursorLocks {
    CursorLocks(OperationContext* opCtx, const NamespaceString& nss, ClientCursorPin& cursorPin);

    // A reference to the shared_ptr so that we drop the virtual collections (via the
    // destructor) after deleting our cursors and releasing our read locks.
    std::shared_ptr<ExternalDataSourceScopeGuard> extDataSourceScopeGuard;
    boost::optional<AutoStatsTracker> statsTracker;
    boost::optional<HandleTransactionResourcesFromStasher> txnResourcesHandler;
};

}  // namespace mongo
