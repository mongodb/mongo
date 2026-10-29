// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace catalog_repair {

/**
 * Drop abandoned idents using two-phase drop at the stable timestamp. Idents may be needed for
 * reads between the oldest and stable timestamps. If successful, returns a ReconcileResult with
 * indexes that need to be rebuilt or builds that need to be restarted.
 *
 * Abandoned internal idents require special handling based on the context known only to the
 * caller. For example, on starting from a previous unclean shutdown, we would always drop all
 * unknown internal idents. If we started from a clean shutdown, the internal idents may contain
 * information for resuming index builds.
 */
[[MONGO_MOD_PUBLIC]]
StatusWith<StorageEngine::ReconcileResult> reconcileCatalogAndIdents(
    OperationContext* opCtx,
    StorageEngine* engine,
    Timestamp stableTs,
    StorageEngine::LastShutdownState lastShutdownState,
    bool forRepair);

}  // namespace catalog_repair
}  // namespace mongo
