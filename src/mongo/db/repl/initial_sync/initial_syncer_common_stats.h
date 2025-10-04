/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/counter.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace repl {
namespace initial_sync_common_stats {

// The number of initial sync attempts that have failed since server startup. Each instance of
// InitialSyncer may run multiple attempts to fulfill an initial sync request that is triggered
// when InitialSyncer::startup() is called.
extern Counter64& initialSyncFailedAttempts;

// The number of initial sync requests that have been requested and failed. Each instance of
// InitialSyncer (upon successful startup()) corresponds to a single initial sync request.
// This value does not include the number of times where a InitialSyncer is created successfully
// but failed in startup().
extern Counter64& initialSyncFailures;

// The number of initial sync requests that have been requested and completed successfully. Each
// instance of InitialSyncer corresponds to a single initial sync request.
extern Counter64& initialSyncCompletes;

void LogInitialSyncAttemptStats(const StatusWith<OpTimeAndWallTime>& attemptResult,
                                bool hasRetries,
                                const BSONObj& stats);

}  // namespace initial_sync_common_stats
}  // namespace repl
}  // namespace mongo
