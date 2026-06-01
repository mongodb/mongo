/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_stats.h"
#include "mongo/db/stats/global_lock_stats.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

class GlobalLockServerStatusSection : public ServerStatusSection {
public:
    GlobalLockServerStatusSection(std::string name, ClusterRole role)
        : ServerStatusSection(name, role), _startedAt(Date_t::now()) {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        const auto snap =
            collectGlobalLockStatsSnapshot(opCtx->getClient()->getServiceContext(), _startedAt);

        BSONObjBuilder ret;
        ret.append("totalTime", snap.totalTimeMicros);

        {
            BSONObjBuilder currentQueueBuilder(ret.subobjStart("currentQueue"));
            currentQueueBuilder.append("total", snap.queuedReaders + snap.queuedWriters);
            currentQueueBuilder.append("readers", snap.queuedReaders);
            currentQueueBuilder.append("writers", snap.queuedWriters);
            currentQueueBuilder.done();
        }

        {
            BSONObjBuilder activeClientsBuilder(ret.subobjStart("activeClients"));
            activeClientsBuilder.append("total", snap.activeReaders + snap.activeWriters);
            activeClientsBuilder.append("readers", snap.activeReaders);
            activeClientsBuilder.append("writers", snap.activeWriters);
            activeClientsBuilder.done();
        }

        ret.done();
        return ret.obj();
    }

private:
    const Date_t _startedAt;
};
auto& globalLockServerStatusSection =
    *ServerStatusSectionBuilder<GlobalLockServerStatusSection>("globalLock").forShard();


class LockStatsServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder ret;

        SingleThreadedLockStats stats;
        reportGlobalLockingStats(&stats);

        stats.report(&ret, true);

        return ret.obj();
    }
};
auto& lockStatsServerStatusSection =
    *ServerStatusSectionBuilder<LockStatsServerStatusSection>("locks").forShard();

}  // namespace
}  // namespace mongo
