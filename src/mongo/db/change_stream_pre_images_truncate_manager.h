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

#pragma once

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <memory>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/change_stream_pre_images_tenant_truncate_markers.h"
#include "mongo/db/change_stream_pre_images_truncate_markers_per_nsUUID.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/concurrent_shared_values_map.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {
/**
 * Manages the truncation of expired pre-images for pre-images collection(s) across all tenants.
 * There is up to one "system.config.preimages" pre-images collection per tenant.
 *
 * In a single-tenant environment, there is only one "system.config.preimages" pre-images
 * collection. In which case, the corresponding truncate markers are mapped to TenantId
 * 'boost::none'.
 */
class PreImagesTruncateManager {
public:
    /*
     * Truncates expired pre-images spanning the tenant's pre-images collection.
     */
    PreImagesTruncateStats truncateExpiredPreImages(OperationContext* opCtx,
                                                    boost::optional<TenantId> tenantId);

    /**
     * Exclusively used when the config.preimages collection associated with 'tenantId' is dropped.
     * All markers will be dropped immediately.
     */
    void dropAllMarkersForTenant(boost::optional<TenantId> tenantId);

    /**
     * Updates truncate markers to account for a newly inserted 'preImage' into the tenant's
     * pre-images collection. If no truncate markers have been created for the 'tenantId's
     * pre-images collection, this is a no-op.
     */
    void updateMarkersOnInsert(OperationContext* opCtx,
                               boost::optional<TenantId> tenantId,
                               const ChangeStreamPreImage& preImage,
                               int64_t bytesInserted);

private:
    friend class PreImagesTruncateManagerTest;

    /**
     * Tries to retrieve truncate markers for the tenant - or initialize the truncate markers if
     * they don't yet exist.
     *
     * Returns a shared_ptr to truncate markers for the tenant's pre-images collection. If truncate
     * markers don't exist (either the collection doesn't exist or the collection was dropped during
     * the initialization process), returns nullptr.
     */
    std::shared_ptr<PreImagesTenantMarkers> _getInitializedMarkersForPreImagesCollection(
        OperationContext* opCtx, boost::optional<TenantId> tenantId);

    /**
     * Returns a shared pointer to 'PreImagesTenantMarkers' installed in the '_tenantMap', provided
     * the truncate markers were successfully installed. Otherwise, returns a null pointer.
     */
    std::shared_ptr<PreImagesTenantMarkers> _createAndInstallMarkers(
        OperationContext* opCtx, boost::optional<TenantId> tenantId);

    ConcurrentSharedValuesMap<boost::optional<TenantId>, PreImagesTenantMarkers> _tenantMap;
};
}  // namespace mongo
