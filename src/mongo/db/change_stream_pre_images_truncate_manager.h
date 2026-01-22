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

#include "mongo/db/change_stream_pre_images_truncate_markers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

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
     * Updates truncate markers to account for a newly inserted 'preImage' into the pre-images
     * collection. If no truncate markers have been created for the 'tenantId's pre-images
     * collection, this is a no-op.
     */
    void updateMarkersOnInsert(OperationContext* opCtx,
                               const ChangeStreamPreImage& preImage,
                               int64_t bytesInserted);

    /**
     * Returns true if the '_truncateMarkers' instance variable is populated, false otherwise.
     */
    bool areTruncateMarkersPopulated_forTest() const;

    /**
     * Test wrapper for calling '_getInitializedMarkersForPreImagesCollection()' from out of unit
     * tests without making the method part of the public API. See that method's description for
     * more details.
     */
    std::shared_ptr<PreImagesTruncateMarkers> getInitializedMarkersForPreImagesCollection_forTest(
        OperationContext* opCtx) {
        return _getInitializedMarkersForPreImagesCollection(opCtx);
    }

private:
    /**
     * Tries to retrieve truncate markers for the pre-images collection - or initialize the truncate
     * markers if they don't yet exist.
     *
     * Returns a shared_ptr to truncate markers for the pre-images collection. Returns a nullptr if
     * the pre-images collection doesn't exist yet.
     */
    std::shared_ptr<PreImagesTruncateMarkers> _getInitializedMarkersForPreImagesCollection(
        OperationContext* opCtx);

    /**
     * Returns a shared pointer to 'PreImagesTenantMarkers', provided the truncate markers were
     * successfully installed. Otherwise, returns a null pointer.
     */
    std::shared_ptr<PreImagesTruncateMarkers> _createAndInstallMarkers(OperationContext* opCtx);

    /**
     * Returns a shared pointer to the currently installed 'PreImagesTenantMarkers' instance, if
     * any. Can return a nullptr.
     * The shared pointer is retrieved under the RWMutex in shared mode, so access to this function
     * is thread-safe.
     */
    std::shared_ptr<PreImagesTruncateMarkers> _getTruncateMarkers() const;

    /**
     * Install a new value in the 'PreImagesTenantMarkers' instance. The new value can be a nullptr.
     * any.
     * The shared pointer is installed under the RWMutex in exclusive mode, so access to method is
     * thread-safe.
     */
    void _setTruncateMarkers(std::shared_ptr<PreImagesTruncateMarkers> truncateMarkers);

    /**
     * Read-write mutex for accessing '_truncateMarkers'. All loads and stores of the
     * '_truncateMarkers' instance in this class must acquire this mutex in the appropriate mode.
     */
    mutable RWMutex _mutex;

    /**
     * Truncate markers for the pre-images collection. The member is populated lazily and can
     * contain a nullptr. This member is protected by '_mutex' and should never be accessed
     * directly, but only via calls to ' _getTruncateMarkers()' and ' _setTruncateMarkers()'.
     */
    std::shared_ptr<PreImagesTruncateMarkers> _truncateMarkers;
};
}  // namespace mongo
