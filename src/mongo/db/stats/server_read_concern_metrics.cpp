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

#include <cstdint>
#include <memory>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_provenance.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/server_read_concern_metrics.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

namespace mongo {
namespace {
const auto ServerReadConcernMetricsDecoration =
    ServiceContext::declareDecoration<ServerReadConcernMetrics>();
}  // namespace

ServerReadConcernMetrics* ServerReadConcernMetrics::get(ServiceContext* service) {
    return &ServerReadConcernMetricsDecoration(service);
}

ServerReadConcernMetrics* ServerReadConcernMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}


void ServerReadConcernMetrics::ReadConcernLevelCounters::recordReadConcern(
    const repl::ReadConcernArgs& readConcernArgs, bool isTransaction) {
    switch (readConcernArgs.getLevel()) {
        case repl::ReadConcernLevel::kAvailableReadConcern:
            invariant(!isTransaction);
            levelAvailableCount.fetchAndAdd(1);
            break;

        case repl::ReadConcernLevel::kLinearizableReadConcern:
            invariant(!isTransaction);
            levelLinearizableCount.fetchAndAdd(1);
            break;

        case repl::ReadConcernLevel::kLocalReadConcern:
            levelLocalCount.fetchAndAdd(1);
            break;

        case repl::ReadConcernLevel::kMajorityReadConcern:
            levelMajorityCount.fetchAndAdd(1);
            break;

        case repl::ReadConcernLevel::kSnapshotReadConcern:
            if (readConcernArgs.getArgsAtClusterTime() &&
                !readConcernArgs.wasAtClusterTimeSelected()) {
                atClusterTimeCount.fetchAndAdd(1);
            } else {
                levelSnapshotCount.fetchAndAdd(1);
            }
            break;

        default:
            MONGO_UNREACHABLE;
    }
}

void ServerReadConcernMetrics::ReadConcernCounters::recordReadConcern(
    const repl::ReadConcernArgs& readConcernArgs, bool isTransaction) {
    if (!readConcernArgs.getProvenance().isClientSupplied() || !readConcernArgs.hasLevel()) {
        if (readConcernArgs.getProvenance().isCustomDefault()) {
            cWRCLevelCount.recordReadConcern(readConcernArgs, isTransaction);
        } else {
            implicitDefaultLevelCount.recordReadConcern(readConcernArgs, isTransaction);
        }

        noLevelCount.fetchAndAdd(1);
        return;
    }

    explicitLevelCount.recordReadConcern(readConcernArgs, isTransaction);
}

void ServerReadConcernMetrics::recordReadConcern(const repl::ReadConcernArgs& readConcernArgs,
                                                 bool isTransaction) {
    auto& ops = isTransaction ? _transactionOps : _nonTransactionOps;
    ops.recordReadConcern(readConcernArgs, isTransaction);
}

void ServerReadConcernMetrics::ReadConcernLevelCounters::updateStats(ReadConcernOps* stats,
                                                                     bool isTransaction) {
    SnapshotOps snapshotOps;
    snapshotOps.setWithoutClusterTime(levelSnapshotCount.load());
    snapshotOps.setWithClusterTime(atClusterTimeCount.load());
    stats->setSnapshot(snapshotOps);

    stats->setLocal(levelLocalCount.load());
    stats->setMajority(levelMajorityCount.load());
    if (!isTransaction) {
        stats->setAvailable(levelAvailableCount.load());
        stats->setLinearizable(levelLinearizableCount.load());
    }
}

void ServerReadConcernMetrics::ReadConcernLevelCounters::updateStats(CWRCReadConcernOps* stats,
                                                                     bool isTransaction) {
    stats->setLocal(levelLocalCount.load());
    stats->setMajority(levelMajorityCount.load());
    if (!isTransaction) {
        stats->setAvailable(levelAvailableCount.load());
    }
}

void ServerReadConcernMetrics::ReadConcernLevelCounters::updateStats(
    ImplicitDefaultReadConcernOps* stats, bool isTransaction) {
    stats->setLocal(levelLocalCount.load());
    if (!isTransaction) {
        stats->setAvailable(levelAvailableCount.load());
    }
}


void ServerReadConcernMetrics::ReadConcernCounters::updateStats(ReadConcernOps* stats,
                                                                bool isTransaction) {
    stats->setNone(noLevelCount.load());
    explicitLevelCount.updateStats(stats, isTransaction);

    NoneInfo noneInfoSection;
    CWRCReadConcernOps cWRCReadConcernOps;
    cWRCLevelCount.updateStats(&cWRCReadConcernOps, isTransaction);

    ImplicitDefaultReadConcernOps implicitDefaultReadConcernOps;
    implicitDefaultLevelCount.updateStats(&implicitDefaultReadConcernOps, isTransaction);

    noneInfoSection.setCWRC(cWRCReadConcernOps);
    noneInfoSection.setImplicitDefault(implicitDefaultReadConcernOps);

    stats->setNoneInfo(noneInfoSection);
}

void ServerReadConcernMetrics::updateStats(ReadConcernStats* stats, OperationContext* opCtx) {
    ReadConcernOps nonTransactionOps;
    _nonTransactionOps.updateStats(&nonTransactionOps, false /* isTransaction */);
    stats->setNonTransactionOps(nonTransactionOps);

    ReadConcernOps transactionOps;
    _transactionOps.updateStats(&transactionOps, true /* isTransaction */);
    stats->setTransactionOps(transactionOps);
}

namespace {
class ReadConcernCountersSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    ~ReadConcernCountersSSS() override = default;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        ReadConcernStats stats;
        ServerReadConcernMetrics::get(opCtx)->updateStats(&stats, opCtx);
        return stats.toBSON();
    }
};
auto readConcernCountersSSS =
    *ServerStatusSectionBuilder<ReadConcernCountersSSS>("readConcernCounters").forShard();
}  // namespace

}  // namespace mongo
