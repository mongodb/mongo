// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {
namespace startup_recovery {
/**
 * After unclean shutdown, change stream collections which utilize truncates may unexpectedly
 * surface parts of previously truncated data.
 *
 * Defines a range where all entries within
 * 'kChangeStreamPostUncleanShutdownExpiryExtensionSeconds' seconds of expiry are truncated after
 * unclean shutdown to guarantee consistent data post recovery.
 */
static constexpr int64_t kChangeStreamPostUncleanShutdownExpiryExtensionSeconds{10};

/**
 * Recovers or repairs all databases from a previous shutdown. May throw a MustDowngrade error
 * if data files are incompatible with the current binary version.
 * The optional parameter `startupTimeElapsedBuilder` is for adding time elapsed of tasks done in
 * this function into one single builder that records the time elapsed during startup. Its default
 * value is nullptr because we only want to time this function when it is called during startup.
 */
void repairAndRecoverDatabases(OperationContext* opCtx,
                               StorageEngine::LastShutdownState lastShutdownState,
                               BSONObjBuilder* startupTimeElapsedBuilder = nullptr);

/**
 * Runs startup recovery after system startup.
 */
void runStartupRecovery(OperationContext* opCtx,
                        StorageEngine::LastShutdownState lastShutdownState,
                        bool afterDataReady = false);

/**
 * Ensures data on the change stream collections is consistent on startup. Only after unclean
 * shutdown is there a risk of inconsistent data.
 *
 * 'lastShutdownState': Indicates whether there was a clean or unclean shutdown before startup.
 * 'isStandalone': Whether the server is started up as a standalone.
 *
 * Change stream pre-images collections use unreplicated, untimestamped truncates to remove expired
 * documents, similar to the oplog. Unlike the oplog, the collections aren't logged, and previously
 * truncated data can unexpectedly surface after an unclean shutdown.
 *
 * To prevent ranges of inconsistent data, preemptively and liberally truncates all documents which
 * may have expired before the crash at startup. Errs on the side of caution by potentially
 * truncating slightly more documents than those expired at the time of shutdown.
 */
void recoverChangeStreamCollections(OperationContext* opCtx,
                                    bool isStandalone,
                                    StorageEngine::LastShutdownState lastShutdownState);

}  // namespace startup_recovery
}  // namespace mongo
