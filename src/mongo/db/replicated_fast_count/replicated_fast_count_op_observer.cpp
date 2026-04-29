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

#include "mongo/db/replicated_fast_count/replicated_fast_count_op_observer.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"

#include <algorithm>
#include <memory>

namespace mongo {
namespace {

class ReplicatedFastCountOpObserver final : public OpObserverNoop {
public:
    NamespaceFilters getNamespaceFilters() const final {
        return {NamespaceFilter::kConfig, NamespaceFilter::kNone};
    }

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) final;

    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) final;
};

bool isFastCountTimestampsNss(const NamespaceString& nss) {
    static const auto timestampsNss = NamespaceString::makeGlobalConfigCollection(
        NamespaceString::kReplicatedFastCountStoreTimestamps);
    return nss == timestampsNss;
}

void scheduleRecordOnCommit(OperationContext* opCtx, const Timestamp& ts) {
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [ts](OperationContext*, boost::optional<Timestamp>) { recordCheckpointAdvanced(ts); });
}

void ReplicatedFastCountOpObserver::onInserts(OperationContext* opCtx,
                                              const CollectionPtr& coll,
                                              std::vector<InsertStatement>::const_iterator begin,
                                              std::vector<InsertStatement>::const_iterator end,
                                              const std::vector<RecordId>&,
                                              std::vector<bool>,
                                              bool,
                                              OpStateAccumulator*) {
    if (begin == end || !isFastCountTimestampsNss(coll->ns())) {
        return;
    }
    Timestamp maxTs;
    for (auto it = begin; it != end; ++it) {
        maxTs = std::max(maxTs, it->doc.getField(replicated_fast_count::kValidAsOfKey).timestamp());
    }
    scheduleRecordOnCommit(opCtx, maxTs);
}

void ReplicatedFastCountOpObserver::onUpdate(OperationContext* opCtx,
                                             const OplogUpdateEntryArgs& args,
                                             OpStateAccumulator*) {
    if (!isFastCountTimestampsNss(args.coll->ns())) {
        return;
    }
    scheduleRecordOnCommit(
        opCtx,
        args.updateArgs->updatedDoc.getField(replicated_fast_count::kValidAsOfKey).timestamp());
}

}  // namespace

void registerReplicatedFastCountOpObserver(ServiceContext* svcCtx) {
    auto* registry = static_cast<OpObserverRegistry*>(svcCtx->getOpObserver());
    registry->addObserver(std::make_unique<ReplicatedFastCountOpObserver>());
}

}  // namespace mongo
