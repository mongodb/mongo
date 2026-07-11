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
const auto getUncommittedFastCountChange =
    RecoveryUnit::Snapshot::declareDecoration<std::shared_ptr<UncommittedFastCountChange>>();

std::shared_ptr<UncommittedFastCountChange>& getUncommittedFastCountChangeFromOpCtx(
    OperationContext* opCtx) {
    return getUncommittedFastCountChange(shard_role_details::getRecoveryUnit(opCtx)->getSnapshot());
}
}  // namespace

const UncommittedFastCountChange& UncommittedFastCountChange::getForRead(OperationContext* opCtx) {
    // TODO SERVER-119919: Re-evaluate why this bypasses reference counting.
    std::shared_ptr<UncommittedFastCountChange>& ptr =
        getUncommittedFastCountChangeFromOpCtx(opCtx);
    if (ptr) {
        return *ptr;
    }

    static UncommittedFastCountChange empty;
    return empty;
}


UncommittedFastCountChange& UncommittedFastCountChange::getForWrite(OperationContext* opCtx) {
    std::shared_ptr<UncommittedFastCountChange>& ptr =
        getUncommittedFastCountChangeFromOpCtx(opCtx);
    if (ptr) {
        return *ptr;
    }

    auto metaChange = std::make_shared<UncommittedFastCountChange>();

    ptr = std::move(metaChange);

    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [](OperationContext* opCtx, boost::optional<Timestamp> commitTime) {
            auto& fn = getFastCountCommitFn();

            invariant(fn, "FastCountCommitFn is not set");

            fn(opCtx, getUncommittedFastCountChangeFromOpCtx(opCtx)->_trackedChanges);
            // The 'RecoveryUnit::Snapshot' is reset on commit, so decorations like the
            // UncommittedFastCountChange don't need manual cleanup.
        });
    return *ptr;
}

CollectionSizeCount UncommittedFastCountChange::find(const UUID& uuid) const {
    auto it = _trackedChanges.find(uuid);
    if (it != _trackedChanges.end()) {
        return it->second;
    }
    return {};
}

void UncommittedFastCountChange::record(const NamespaceString& nss,
                                        const UUID& uuid,
                                        int64_t numDelta,
                                        int64_t sizeDelta) {
    if (!isReplicatedFastCountEligible(nss)) {
        return;
    }
    if (numDelta == 0 && sizeDelta == 0) {
        return;
    }

    auto& collChanges = _trackedChanges[uuid];
    collChanges.count += numDelta;
    collChanges.size += sizeDelta;
}

}  // namespace mongo

