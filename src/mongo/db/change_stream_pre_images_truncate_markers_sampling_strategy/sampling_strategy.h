// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/change_stream_pre_images_truncate_markers_per_nsUUID.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrent_shared_values_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::pre_image_marker_initialization_internal {
/**
 * The RecordId and wall time extracted from a pre-image. Comprises a pre-image 'sample'.
 */
using RecordIdAndWallTime = CollectionTruncateMarkers::RecordIdAndWallTime;

/**
 * Pre-images samples for multiple collections, keyed by collection UUID.
 */
using SamplesMap = stdx::unordered_map<UUID, std::vector<RecordIdAndWallTime>, UUID::Hash>;

/**
 * Pre-images truncate markers, keyed by collection UUID.
 */
using MarkersMap = ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash>;

/**
 * Performs a loose reverse scan over the preimages collection and tracks the highest
 * timestamp/applyOpsIndex combination for every distinct 'nsUUID' value. Returns the
 * 'CollectionTruncateMarkers::RecordIdAndWallTime' for the most recent pre-image per each
 * 'nsUUID' in the pre-images collection.
 */
stdx::unordered_map<UUID, RecordIdAndWallTime, UUID::Hash> sampleLastRecordPerNsUUID(
    OperationContext* opCtx, const CollectionAcquisition& preImagesCollection);

/**
 * Returns the total number of pre-image samples contained in 'samplesMap' by summing
 * the sizes of all per-UUID sample vectors.
 */
int64_t countTotalSamples(const SamplesMap& samplesMap);

/**
 * Interface for strategies that populate truncate markers for the pre-images collection
 * based on sampling.
 */
class SamplingStrategy {
public:
    virtual ~SamplingStrategy() = default;

    /**
     * Populates the 'markersMap' with truncate markers covering the entire pre-images collection.
     * Only pre-images visible in the thread's initial snapshot of the pre-images collection are
     * guaranteed to be covered.
     * Returns true if the strategy performs sampling and updates `markersMap`, otherwise false.
     */
    [[nodiscard]]
    virtual bool performSampling(OperationContext* opCtx,
                                 const CollectionAcquisition& preImagesCollection,
                                 MarkersMap& markersMap) = 0;

protected:
    /**
     * Use the per-nsUUID samples in 'samples' to generate and install in 'markersMap' the initial
     * sets of whole markers for each distinct 'nsUUID' value. Then fixes the entries in
     * 'markersMap' so the aggregate number of records and bytes across the map make up the expected
     * number of records
     * ('numRecords') and bytes ('dataSize') in the pre-images collection.
     */
    void installMarkersInMapFromSamples(OperationContext* opCtx,
                                        const CollectionAcquisition& preImagesCollection,
                                        const SamplesMap& samples,
                                        int64_t numRecords,
                                        int64_t dataSize,
                                        int32_t minBytesPerMarker,
                                        uint64_t randomSamplesPerMarker,
                                        double estimatedRecordsPerMarker,
                                        double estimatedBytesPerMarker,
                                        MarkersMap& markersMap);

    /**
     * Given the expected 'numRecords' and 'dataSize' of the pre-images collection, and the number
     * of 'recordsInMarkersMap' and 'bytesInMarkersMap', distributes the difference across truncate
     * markers so the resulting 'markersMap' accounts for the total 'numRecords' and 'dataSize'.
     */
    void distributeUnaccountedRecordsAndBytes(const stdx::unordered_set<UUID, UUID::Hash>& nsUUIDs,
                                              const UUID& preImagesCollectionUUID,
                                              int64_t numRecords,
                                              int64_t dataSize,
                                              int64_t recordsInMarkersMap,
                                              int64_t bytesInMarkersMap,
                                              MarkersMap& markersMap);
};

/**
 * SamplingStrategy that first runs a primary strategy and, if that strategy reports
 * no work (returns false), runs a fallback strategy instead.
 */
class PrimaryWithFallbackSamplingStrategy : public SamplingStrategy {
public:
    explicit PrimaryWithFallbackSamplingStrategy(std::unique_ptr<SamplingStrategy> primaryStrategy,
                                                 std::unique_ptr<SamplingStrategy> fallbackStrategy)
        : _primaryStrategy(std::move(primaryStrategy)),
          _fallbackStrategy(std::move(fallbackStrategy)) {}

    /**
     * Calls the primary strategy's performSampling(). If it returns false, calls the
     * fallback strategy's performSampling().
     *
     * Returns true if either strategy performs sampling and updates `markersMap`, otherwise false.
     */
    [[nodiscard]]
    bool performSampling(OperationContext* opCtx,
                         const CollectionAcquisition& preImagesCollection,
                         MarkersMap& markersMap) override;

private:
    std::unique_ptr<SamplingStrategy> _primaryStrategy;
    std::unique_ptr<SamplingStrategy> _fallbackStrategy;
};
}  // namespace mongo::pre_image_marker_initialization_internal
