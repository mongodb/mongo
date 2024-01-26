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

#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_pre_images_tenant_truncate_markers.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
PreImagesTruncateStats PreImagesTruncateManager::truncateExpiredPreImages(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionAcquisition& preImagesCollection) {
    auto tenantTruncateMarkers =
        _fetchOrCreateMarkersForPreImagesCollection(opCtx, tenantId, preImagesCollection);
    return tenantTruncateMarkers->truncateExpiredPreImages(opCtx, preImagesCollection);
}

void PreImagesTruncateManager::dropAllMarkersForTenant(boost::optional<TenantId> tenantId) {
    _tenantMap.erase(tenantId);
}

void PreImagesTruncateManager::updateMarkersOnInsert(OperationContext* opCtx,
                                                     boost::optional<TenantId> tenantId,
                                                     const ChangeStreamPreImage& preImage,
                                                     int64_t bytesInserted) {
    dassert(bytesInserted != 0);
    auto nsUUID = preImage.getId().getNsUUID();
    auto wallTime = preImage.getOperationTime();
    auto recordId = change_stream_pre_image_util::toRecordId(preImage.getId());

    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [this,
         tenantId = std::move(tenantId),
         nsUUID = std::move(nsUUID),
         recordId = std::move(recordId),
         bytesInserted,
         wallTime](OperationContext* opCtx, boost::optional<Timestamp>) {
            auto tenantTruncateMarkers = _tenantMap.find(tenantId);
            if (!tenantTruncateMarkers) {
                return;
            }

            tenantTruncateMarkers->updateOnInsert(recordId, nsUUID, wallTime, bytesInserted);
        });
}

std::shared_ptr<PreImagesTenantMarkers>
PreImagesTruncateManager::_fetchOrCreateMarkersForPreImagesCollection(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionAcquisition& preImagesCollection) {
    auto tenantMarkers = _tenantMap.find(tenantId);
    if (tenantMarkers) {
        return tenantMarkers;
    }

    // Create and install markers for the tenant.

    // (A) Create 'PreImagesTenantMarkers' for the tenant's pre-images collection. The
    // 'baseMarkers' are created using the thread's snapshot of the pre-images collection - meaning
    // they might not account for concurrent pre-image insertions beyond the current snapshot.
    auto baseMarkers = PreImagesTenantMarkers::createMarkers(opCtx, tenantId, preImagesCollection);

    // (B) Install the 'baseMarkers' into the map. From now on, pre-image inserts will
    // update the 'tenantMarkers'.
    tenantMarkers = _tenantMap.getOrEmplace(tenantId, std::move(baseMarkers));

    // (C) Ensure that 'tenantMarkers' account for the most recent pre-image inserts - specifically,
    // any inserts that occurred between (A) and (B) at a later snapshot than the snapshot used in
    // (A). Otherwise, the truncate markers won't know there are pre-images past the snapshot from
    // (A) until a new insert comes along for each pre-image nsUUID out of date.
    tenantMarkers->refreshMarkers(opCtx, std::move(preImagesCollection));

    return tenantMarkers;
}

}  // namespace mongo
