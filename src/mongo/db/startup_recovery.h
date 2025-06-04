/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/storage/storage_engine.h"

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
                        StorageEngine::LastShutdownState lastShutdownState);

/**
 * Ensures data on the change stream collections is consistent on startup. Only after unclean
 * shutdown is there a risk of inconsistent data.
 *
 * 'lastShutdownState': Indicates whether there was a clean or unclean shutdown before startup.
 * 'isStandalone': Whether the server is started up as a standalone.
 *
 * Both change stream change collections and change stream pre-images collections use unreplicated,
 * untimestamped truncates to remove expired documents, similar to the oplog. Unlike the oplog, the
 * collections aren't logged, and previously truncated data can unexpectedly surface after an
 * unclean shutdown.
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
