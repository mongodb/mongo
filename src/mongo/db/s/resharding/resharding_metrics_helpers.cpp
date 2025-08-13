/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_metrics_helpers.h"

#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
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
