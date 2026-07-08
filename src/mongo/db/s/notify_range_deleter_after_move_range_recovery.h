/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/service_context.h"

namespace mongo {

/**
 * Kicks off asynchronous work that notifies RangeDeleterService once every MoveRangeCoordinator
 * recovered for the current term has finished, resolving the recovery job registered on their
 * behalf in migrationutil::registerMigrationRecoveryJobs(). Returns immediately; the wait and
 * notification happen on a separate executor.
 *
 * Invoked from ShardingCoordinatorService::_onServiceInitialization() via the
 * moveRangeRecoveryNotifier constructor parameter, rather than called directly, so unit tests that
 * construct the service without a fully initialized sharding environment (Grid, RangeDeleter) can
 * default to a no-op. Only mongod_main.cpp, which wires up the real production instance, should
 * pass this function in. Note that this hook fires too late (during onStepUpComplete) to register
 * the job itself; that must happen earlier, during onStepUpBegin, which is why
 * registerMigrationRecoveryJobs() owns that half.
 */
void asyncNotifyRangeDeleterAfterMoveRangeRecovery(ServiceContext* serviceContext);

}  // namespace mongo
