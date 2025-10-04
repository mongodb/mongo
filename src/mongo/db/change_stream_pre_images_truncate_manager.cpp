/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/change_stream_pre_images_truncate_manager.h"

#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_pre_images_tenant_truncate_markers.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

MONGO_FAIL_POINT_DEFINE(preImagesTruncateOnlyOnSecondaries);

PreImagesTruncateStats PreImagesTruncateManager::truncateExpiredPreImages(OperationContext* opCtx) {
    // Pre-images collections can multiply the amount of user data inserted and deleted
    // on each node. It is imperative that truncate marker generation and pre-image removal are
    // prioritized so they can keep up with inserts and prevent users from running out of disk
    // space.
    ScopedAdmissionPriority<ExecutionAdmissionContext> skipAdmissionControl(
        opCtx, AdmissionContext::Priority::kExempt);

    try {
        auto tenantTruncateMarkers = _getInitializedMarkersForPreImagesCollection(opCtx);
        if (!tenantTruncateMarkers) {
            return {};
        }

        return tenantTruncateMarkers->truncateExpiredPreImages(opCtx);
    } catch (const ExceptionFor<ErrorCodes::InterruptedDueToStorageChange>& ex) {
        LOGV2_INFO(9023601,
                   "Pre-image truncation process interrupted due to storage change. Clearing "
                   "stale in-memory state",
                   "reason"_attr = ex.toStatus());
        // Pre-image truncate markers across tenants were created with an old storage engine and are
        // no longer reliable.
        _tenantMap.clear();
        throw;
    } catch (const DBException&) {
        throw;
    }
}

void PreImagesTruncateManager::updateMarkersOnInsert(OperationContext* opCtx,
                                                     const ChangeStreamPreImage& preImage,
                                                     int64_t bytesInserted) {
    dassert(bytesInserted != 0);
    auto nsUUID = preImage.getId().getNsUUID();
    auto wallTime = preImage.getOperationTime();
    auto recordId = change_stream_pre_image_util::toRecordId(preImage.getId());

    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [this, nsUUID = std::move(nsUUID), recordId = std::move(recordId), bytesInserted, wallTime](
            OperationContext* opCtx, boost::optional<Timestamp>) {
            // TODO SERVER-109269: Remove _tenantMap usage.
            auto tenantTruncateMarkers = _tenantMap.find(boost::none);
            if (!tenantTruncateMarkers) {
                return;
            }

            tenantTruncateMarkers->updateOnInsert(recordId, nsUUID, wallTime, bytesInserted);
        });
}

std::shared_ptr<PreImagesTenantMarkers>
PreImagesTruncateManager::_getInitializedMarkersForPreImagesCollection(OperationContext* opCtx) {
    auto tenantMarkers = _tenantMap.find(boost::none);
    if (tenantMarkers) {
        return tenantMarkers;
    }

    // Truncate markers need to be initialized for the pre-images collection. Truncate markers
    // should track the highest seen RecordId and wall time across pre-images to guarantee all
    // pre-images are eventually truncated.
    //
    // Minimize the likelihood that pre-images inserted during initialization are unaccounted for by
    // relaxing constraints (to view the most up to date data). This is safe even during secondary
    // batch application because the truncate marker mechanism is designed to handle unserialized
    // inserts of pre-images.
    ON_BLOCK_EXIT([opCtx, isEnforcingConstraints = opCtx->isEnforcingConstraints()] {
        opCtx->setEnforceConstraints(isEnforcingConstraints);
    });
    opCtx->setEnforceConstraints(false);

    // Guard against an early exit with incomplete truncate markers installed in the '_tenantMap'.
    ScopeGuard uninstallIncompleteTruncateMarkers([&] { _tenantMap.erase(boost::none); });
    try {
        // (A) Create 'PreImagesTenantMarkers' for the pre-images collection and install
        // them into the _tenantMap. The 'tenantMarkers' might not account for concurrent pre-image
        // insertions beyond the snapshot used to create the markers.
        tenantMarkers = _createAndInstallMarkers(opCtx);
        if (!tenantMarkers) {
            return nullptr;
        }
        LOGV2_DEBUG(9023602,
                    1,
                    "Installed pre-image truncate markers in tenant map. Markers must be finalized "
                    "for safe truncation",
                    "preImagesCollectionUUID"_attr = tenantMarkers->getPreImagesCollectionUUID());

        // (B) Ensure that 'tenantMarkers' account for the most recent pre-image inserts -
        // specifically, any inserts that occurred during (A) at a later snapshot than the
        // snapshot used to create the markers in (A). Otherwise, the truncate markers won't know
        // there are pre-images past the snapshot from (A) until a new insert comes along for each
        // pre-image nsUUID out of date.
        tenantMarkers->refreshMarkers(opCtx);

        LOGV2(9023600,
              "Completed initialization of pre-image tenant truncate markers",
              "preImagesCollectionUUID"_attr = tenantMarkers->getPreImagesCollectionUUID());
    } catch (const DBException& ex) {
        LOGV2_INFO(9030100,
                   "Failed to complete pre-image truncate marker initialization",
                   "reason"_attr = ex.toStatus());
        throw;
    }

    uninstallIncompleteTruncateMarkers.dismiss();
    return tenantMarkers;
}

std::shared_ptr<PreImagesTenantMarkers> PreImagesTruncateManager::_createAndInstallMarkers(
    OperationContext* opCtx) {
    return writeConflictRetry(
        opCtx,
        "Generating and installing pre image truncate markers for tenant",
        NamespaceString::kChangeStreamPreImagesNamespace,
        [&]() -> std::shared_ptr<PreImagesTenantMarkers> {
            const auto preImagesCollection =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest(
                                      NamespaceString::kChangeStreamPreImagesNamespace,
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx),
                                      AcquisitionPrerequisites::kRead),
                                  MODE_IS);

            if (!preImagesCollection.exists() ||
                (MONGO_unlikely(preImagesTruncateOnlyOnSecondaries.shouldFail()) &&
                 repl::ReplicationCoordinator::get(opCtx)->getMemberState() ==
                     repl::MemberState::RS_PRIMARY)) {
                return nullptr;
            }

            // Serialize installation under the collection's lock to guarantee the markers installed
            // aren't for a stale, dropped version of the collection.
            auto baseMarkers = PreImagesTenantMarkers::createMarkers(opCtx, preImagesCollection);
            auto tenantMapEntry = _tenantMap.getOrEmplace(boost::none, std::move(baseMarkers));
            return tenantMapEntry;
        });
}

}  // namespace mongo
