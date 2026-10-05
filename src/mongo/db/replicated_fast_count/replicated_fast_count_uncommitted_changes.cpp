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
    RecoveryUnit::Snapshot::declareDecoration<std::unique_ptr<UncommittedFastCountChanges>>();

std::unique_ptr<UncommittedFastCountChanges>& getUncommittedFastCountChangeFromOpCtx(
    OperationContext* opCtx) {
    return getUncommittedFastCountChanges(
        shard_role_details::getRecoveryUnit(opCtx)->getSnapshot());
}
}  // namespace

CollectionSizeCount UncommittedFastCountChanges::find(OperationContext* opCtx, UUID uuid) {
    const std::unique_ptr<UncommittedFastCountChanges>& ptr =
        getUncommittedFastCountChangeFromOpCtx(opCtx);
    if (ptr) {
        return ptr->_find(uuid);
    }
    return {};
}

void UncommittedFastCountChanges::record(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         UUID uuid,
                                         UncommittedFastCountChange change) {
    std::unique_ptr<UncommittedFastCountChanges>& ptr =
        getUncommittedFastCountChangeFromOpCtx(opCtx);
    if (!ptr) {
        ptr = std::make_unique<UncommittedFastCountChanges>();

        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [](OperationContext* opCtx, boost::optional<Timestamp> commitTime) {
                auto& fn = getFastCountCommitFn();

                invariant(fn, "FastCountCommitFn is not set");

                auto& ptr = getUncommittedFastCountChangeFromOpCtx(opCtx);
                invariant(ptr, "UncommittedFastCountChanges instance is not set");
                fn(opCtx, ptr->_trackedChanges);
                // The 'RecoveryUnit::Snapshot' is reset on commit, so decorations like the
                // UncommittedFastCountChanges don't need manual cleanup.
            });
    }

    ptr->_record(nss, uuid, change);
}

CollectionSizeCount UncommittedFastCountChanges::_find(UUID uuid) const {
    auto it = _trackedChanges.find(uuid);
    if (it != _trackedChanges.end()) {
        return it->second.delta;
    }
    return {};
}

void UncommittedFastCountChanges::_record(const NamespaceString& nss,
                                          UUID uuid,
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
