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

#include "mongo/db/stats/global_lock_stats.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/assert_util.h"

#include <array>
#include <mutex>

namespace mongo {

GlobalLockStatsSnapshot collectGlobalLockStatsSnapshot(ServiceContext* svcCtx, Date_t startedAt) {
    std::array<int64_t, 5> counts{};
    static_assert(Locker::kQueuedWriter < counts.size(),
                  "GlobalLockStatsSnapshot expects Locker::ClientState to fit in 5 buckets");

    for (ServiceContext::LockedClientsCursor cursor(svcCtx); Client* client = cursor.next();) {
        invariant(client);
        std::unique_lock<Client> uniqueLock(*client);

        const OperationContext* clientOpCtx = client->getOperationContext();
        const auto state = clientOpCtx
            ? shard_role_details::getLocker(clientOpCtx)->getClientState()
            : Locker::kInactive;
        invariant(static_cast<size_t>(state) < counts.size());
        ++counts[state];
    }

    GlobalLockStatsSnapshot snap;
    snap.activeReaders = counts[Locker::kActiveReader];
    snap.activeWriters = counts[Locker::kActiveWriter];
    snap.queuedReaders = counts[Locker::kQueuedReader];
    snap.queuedWriters = counts[Locker::kQueuedWriter];

    const auto elapsed = Date_t::now() - startedAt;
    snap.totalTimeMicros = durationCount<Microseconds>(elapsed);
    return snap;
}

}  // namespace mongo
