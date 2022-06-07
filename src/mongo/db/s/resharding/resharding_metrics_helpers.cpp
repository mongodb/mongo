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
#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {
namespace resharding_metrics {

namespace {
void onCriticalSectionErrorThrows(OperationContext* opCtx, const StaleConfigInfo& info) {
    const auto& operationType = info.getDuringOperationType();
    if (!operationType) {
        return;
    }
    AutoGetCollection autoColl(opCtx, info.getNss(), MODE_IS);
    auto csr = CollectionShardingRuntime::get(opCtx, info.getNss());
    auto metadata = csr->getCurrentMetadataIfKnown();
    if (!metadata || !metadata->isSharded()) {
        return;
    }
    const auto& reshardingFields = metadata->getReshardingFields();
    if (!reshardingFields) {
        return;
    }
    auto stateMachine =
        resharding::tryGetReshardingStateMachine<ReshardingDonorService,
                                                 ReshardingDonorService::DonorStateMachine,
                                                 ReshardingDonorDocument>(
            opCtx, reshardingFields->getReshardingUUID());
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


void onCriticalSectionError(OperationContext* opCtx, const StaleConfigInfo& info) noexcept {
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
