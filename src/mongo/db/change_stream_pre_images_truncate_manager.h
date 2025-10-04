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

#include "mongo/db/change_stream_pre_images_tenant_truncate_markers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/util/concurrent_shared_values_map.h"

#include <cstdint>
#include <memory>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {
/**
 * Manages the truncation of expired pre-images for the pre-images collection.
 */
class PreImagesTruncateManager {
public:
    /*
     * Truncates expired pre-images spanning the pre-images collection.
     */
    PreImagesTruncateStats truncateExpiredPreImages(OperationContext* opCtx);

    /**
     * Updates truncate markers to account for a newly inserted 'preImage' into the tenant's
     * pre-images collection. If no truncate markers have been created for the 'tenantId's
     * pre-images collection, this is a no-op.
     */
    void updateMarkersOnInsert(OperationContext* opCtx,
                               const ChangeStreamPreImage& preImage,
                               int64_t bytesInserted);

private:
    friend class PreImagesTruncateManagerTest;

    /**
     * Tries to retrieve truncate markers for the pre-images collection - or initialize the truncate
     * markers if they don't yet exist.
     *
     * Returns a shared_ptr to truncate markers for the pre-images collection. Returns a nullptr if
     * the pre-images collection doesn't exist yet.
     */
    std::shared_ptr<PreImagesTenantMarkers> _getInitializedMarkersForPreImagesCollection(
        OperationContext* opCtx);

    /**
     * Returns a shared pointer to 'PreImagesTenantMarkers', provided the truncate markers were
     * successfully installed. Otherwise, returns a null pointer.
     */
    std::shared_ptr<PreImagesTenantMarkers> _createAndInstallMarkers(OperationContext* opCtx);

    // TODO SERVER-109269: Remove map. Until then, only one entry should ever exist, for tenantId
    // boost::none.
    ConcurrentSharedValuesMap<boost::optional<TenantId>, PreImagesTenantMarkers> _tenantMap;
};
}  // namespace mongo
