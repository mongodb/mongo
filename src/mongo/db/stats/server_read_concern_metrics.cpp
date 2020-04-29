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

#include "mongo/platform/basic.h"

#include "mongo/db/stats/server_read_concern_metrics.h"

#include "mongo/db/commands/server_status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

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

void ServerReadConcernMetrics::recordReadConcern(const repl::ReadConcernArgs& readConcernArgs,
                                                 bool isTransaction) {
    auto& ops = isTransaction ? _transactionOps : _nonTransactionOps;

    if (!readConcernArgs.hasLevel()) {
        ops.noLevelCount.fetchAndAdd(1);
        return;
    }

    switch (readConcernArgs.getLevel()) {
        case repl::ReadConcernLevel::kAvailableReadConcern:
            invariant(!isTransaction);
            ops.levelAvailableCount.fetchAndAdd(1);
            break;

        case repl::ReadConcernLevel::kLinearizableReadConcern:
            invariant(!isTransaction);
            ops.levelLinearizableCount.fetchAndAdd(1);
            break;

        case repl::ReadConcernLevel::kLocalReadConcern:
            ops.levelLocalCount.fetchAndAdd(1);
            break;

        case repl::ReadConcernLevel::kMajorityReadConcern:
            ops.levelMajorityCount.fetchAndAdd(1);
            break;

        case repl::ReadConcernLevel::kSnapshotReadConcern:
            if (readConcernArgs.getArgsAtClusterTime() &&
                !readConcernArgs.wasAtClusterTimeSelected()) {
                ops.atClusterTimeCount.fetchAndAdd(1);
            } else {
                ops.levelSnapshotCount.fetchAndAdd(1);
            }
            break;

        default:
            MONGO_UNREACHABLE;
    }
}

void ServerReadConcernMetrics::updateStats(ReadConcernStats* stats, OperationContext* opCtx) {
    ReadConcernOps nonTransactionOps;
    SnapshotOps nonTransactionSnapshotOps;
    nonTransactionSnapshotOps.setWithoutClusterTime(_nonTransactionOps.levelSnapshotCount.load());
    nonTransactionSnapshotOps.setWithClusterTime(_nonTransactionOps.atClusterTimeCount.load());
    nonTransactionOps.setNone(_nonTransactionOps.noLevelCount.load());
    nonTransactionOps.setAvailable(_nonTransactionOps.levelAvailableCount.load());
    nonTransactionOps.setLinearizable(_nonTransactionOps.levelLinearizableCount.load());
    nonTransactionOps.setLocal(_nonTransactionOps.levelLocalCount.load());
    nonTransactionOps.setMajority(_nonTransactionOps.levelMajorityCount.load());
    nonTransactionOps.setSnapshot(nonTransactionSnapshotOps);
    stats->setNonTransactionOps(nonTransactionOps);

    ReadConcernOps transactionOps;
    SnapshotOps transactionSnapshotOps;
    transactionSnapshotOps.setWithoutClusterTime(_transactionOps.levelSnapshotCount.load());
    transactionSnapshotOps.setWithClusterTime(_transactionOps.atClusterTimeCount.load());
    transactionOps.setNone(_transactionOps.noLevelCount.load());
    transactionOps.setLocal(_transactionOps.levelLocalCount.load());
    transactionOps.setMajority(_transactionOps.levelMajorityCount.load());
    transactionOps.setSnapshot(transactionSnapshotOps);
    stats->setTransactionOps(transactionOps);
}

namespace {
class ReadConcernCountersSSS : public ServerStatusSection {
public:
    ReadConcernCountersSSS() : ServerStatusSection("readConcernCounters") {}

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

} ReadConcernCountersSSS;
}  // namespace

}  // namespace mongo
