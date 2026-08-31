// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/replicated_fast_count_uncommitted_changes.h"

#include "mongo/db/replicated_fast_count/replicated_fast_count_committer.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"
#include "mongo/db/shard_role/transaction_resources.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {
// Decoration on the Snapshot to ensure the uncommitted changes are preserved across the lifetime of
// a multi-document transaction.
const auto getUncommittedFastCountChanges =
    RecoveryUnit::Snapshot::declareDecoration<std::shared_ptr<UncommittedFastCountChanges>>();

std::shared_ptr<UncommittedFastCountChanges>& getUncommittedFastCountChangeFromOpCtx(
    OperationContext* opCtx) {
    return getUncommittedFastCountChanges(
        shard_role_details::getRecoveryUnit(opCtx)->getSnapshot());
}
}  // namespace

const UncommittedFastCountChanges& UncommittedFastCountChanges::getForRead(
    OperationContext* opCtx) {
    // TODO SERVER-119919: Re-evaluate why this bypasses reference counting.
    std::shared_ptr<UncommittedFastCountChanges>& ptr =
        getUncommittedFastCountChangeFromOpCtx(opCtx);
    if (ptr) {
        return *ptr;
    }

    static UncommittedFastCountChanges empty;
    return empty;
}


UncommittedFastCountChanges& UncommittedFastCountChanges::getForWrite(OperationContext* opCtx) {
    std::shared_ptr<UncommittedFastCountChanges>& ptr =
        getUncommittedFastCountChangeFromOpCtx(opCtx);
    if (ptr) {
        return *ptr;
    }

    auto changes = std::make_shared<UncommittedFastCountChanges>();

    ptr = std::move(changes);

    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [](OperationContext* opCtx, boost::optional<Timestamp> commitTime) {
            auto& fn = getFastCountCommitFn();

            invariant(fn, "FastCountCommitFn is not set");

            fn(opCtx, getUncommittedFastCountChangeFromOpCtx(opCtx)->_trackedChanges);
            // The 'RecoveryUnit::Snapshot' is reset on commit, so decorations like the
            // UncommittedFastCountChanges don't need manual cleanup.
        });
    return *ptr;
}

CollectionSizeCount UncommittedFastCountChanges::find(const UUID& uuid) const {
    auto it = _trackedChanges.find(uuid);
    if (it != _trackedChanges.end()) {
        return it->second.delta;
    }
    return {};
}

void UncommittedFastCountChanges::record(const NamespaceString& nss,
                                         const UUID& uuid,
                                         UncommittedFastCountChange change) {
    if (!isReplicatedFastCountEligible(nss)) {
        return;
    }
    if (change.delta.count == 0 && change.delta.size == 0) {
        return;
    }

    invariant(change.recordStore,
              fmt::format("Cannot record a fast count change without a RecordStore for {}",
                          nss.toStringForErrorMsg()));

    auto& collChanges = _trackedChanges[uuid];
    if (!collChanges.recordStore) {
        collChanges.recordStore = change.recordStore;
    }
    collChanges.delta = collChanges.delta + change.delta;
}

}  // namespace mongo

