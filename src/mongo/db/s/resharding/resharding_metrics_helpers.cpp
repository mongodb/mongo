// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_metrics_helpers.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/resharding/local_resharding_operations_registry.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace resharding_metrics {
namespace {

boost::optional<UUID> tryGetReshardingUUID(OperationContext* opCtx, const NamespaceString& nss) {
    // The user CRUD opCtx has no coordinator-pinned VersionContext, so a flag gate here would
    // race with FCV transitions. Consult the registry (populated by the op observer under the
    // coordinator's pinned OFCV) first; fall back to the legacy reshardingFields lookup for
    // pre-registry ops.
    if (auto op = LocalReshardingOperationsRegistry::get().getOperation(nss)) {
        return op->metadata.getReshardingUUID();
    }

    // If the metadata is not known (because this is a secondary that stepped up during the critical
    // section), the metrics will not be incremented. The resharding metrics already do not attempt
    // to restore the number of reads/writes done on a previous primary during a critical section,
    // so this is considered acceptable.
    const auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx, nss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    if (!metadata || !metadata->isSharded()) {
        return boost::none;
    }
    const auto& reshardingFields = metadata->getReshardingFields();
    if (!reshardingFields) {
        return boost::none;
    }
    return reshardingFields->getReshardingUUID();
}

void onCriticalSectionErrorThrows(OperationContext* opCtx, const StaleConfigInfo& info) {
    const auto& operationType = info.getDuringOperationType();
    if (!operationType) {
        return;
    }
    auto reshardingId = tryGetReshardingUUID(opCtx, info.getNss());
    if (!reshardingId) {
        return;
    }
    auto stateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingDonorService,
                                                 ReshardingDonorService::DonorStateMachine,
                                                 ReshardingDonorDocument>(opCtx, *reshardingId);
    if (!stateMachine) {
        return;
    }
    switch (*operationType) {
        case StaleConfigInfo::OperationType::kWrite:
            (*stateMachine)->onWriteDuringCriticalSection();
            return;
        case StaleConfigInfo::OperationType::kRead:
            (*stateMachine)->onReadDuringCriticalSection();
            return;
    }
}
}  // namespace


void onCriticalSectionError(OperationContext* opCtx, const StaleConfigInfo& info) {
    try {
        onCriticalSectionErrorThrows(opCtx, info);
    } catch (const DBException& e) {
        LOGV2(6437201,
              "Unable to record resharding critical section metrics for the current operation",
              "reason"_attr = redact(e.toStatus()));
    }
}

}  // namespace resharding_metrics
}  // namespace mongo
