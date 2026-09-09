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

#include <memory>
#include <string_view>

namespace mongo {
namespace {

class ReplicatedFastCountOpObserver final : public OpObserverNoop {
public:
    NamespaceFilters getNamespaceFilters() const final {
        return {NamespaceFilter::kNone, NamespaceFilter::kNone};
    }

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

    void onContainerInsert(OperationContext* opCtx,
                           std::string_view ident,
                           std::span<const std::span<const char>> keys,
                           std::span<const char> value) final {
        recordContainerWriteForFastCountTimestamp(opCtx, ident, value);
    }

    void onContainerInsert(OperationContext* opCtx,
                           std::string_view ident,
                           int64_t base,
                           std::span<const std::span<const char>> values) final {
        if (!values.empty()) {
            // All key-value pairs should have the same timestamp, use the last one for recording
            recordContainerWriteForFastCountTimestamp(opCtx, ident, values.back());
        }  // Else, no-op
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

void scheduleRecordOnCommit(OperationContext* opCtx, const Timestamp& ts) {
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [ts](OperationContext*, boost::optional<Timestamp>) { recordCheckpointAdvanced(ts); });
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
