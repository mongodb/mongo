// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
        const auto snap = collectGlobalLockStatsSnapshot(_startedAt);

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
