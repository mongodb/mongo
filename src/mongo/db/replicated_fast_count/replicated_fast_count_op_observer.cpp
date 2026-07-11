// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/replicated_fast_count_op_observer.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/recovery_unit.h"

#include <algorithm>
#include <memory>
#include <string_view>

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

    void onContainerInsert(OperationContext* opCtx,
                           std::string_view ident,
                           int64_t key,
                           std::span<const char> value) final {
        recordContainerWriteForFastCountTimestamp(opCtx, ident, value);
    }

    void onContainerInsert(OperationContext* opCtx,
                           std::string_view ident,
                           std::span<const char> key,
                           std::span<const char> value) final {
        recordContainerWriteForFastCountTimestamp(opCtx, ident, value);
    }

    void onContainerUpdate(OperationContext* opCtx,
                           std::string_view ident,
                           int64_t key,
                           std::span<const char> value) final {
        recordContainerWriteForFastCountTimestamp(opCtx, ident, value);
    }

    void onContainerUpdate(OperationContext* opCtx,
                           std::string_view ident,
                           std::span<const char> key,
                           std::span<const char> value) final {
        recordContainerWriteForFastCountTimestamp(opCtx, ident, value);
    }
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

void recordContainerWriteForFastCountTimestamp(OperationContext* opCtx,
                                               std::string_view ident,
                                               std::span<const char> valueBytes) {
    if (ident != ::mongo::ident::kFastCountMetadataStoreTimestamps) {
        return;
    }
    if (valueBytes.size() == 0) {
        // Do nothing for container deletes. These are not currently used for the Timestamps
        // container.
        return;
    }
    BSONObj doc(valueBytes.data());
    const auto ts = doc.getField(replicated_fast_count::kValidAsOfKey).timestamp();
    if (ts.isNull()) {
        return;
    }
    scheduleRecordOnCommit(opCtx, ts);
}

}  // namespace mongo
