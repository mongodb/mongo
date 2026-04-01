/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/change_stream_pre_images_truncate_markers.h"

#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/change_stream_pre_image_id_util.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/equal_step_sampling_strategy.h"
#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/random_sampling_strategy.h"
#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/sampling_strategy.h"
#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/scanning_sampling_strategy.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <cstdint>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

class MaybeUnreplicatedPreImageTruncateBlock {
public:
    explicit MaybeUnreplicatedPreImageTruncateBlock(OperationContext* opCtx,
                                                    bool useReplicatedTruncates) {
        if (!useReplicatedTruncates) {
            // Use unreplicated truncates only when required.
            _uwb.emplace(opCtx);
        }
    }

    MaybeUnreplicatedPreImageTruncateBlock(const MaybeUnreplicatedPreImageTruncateBlock&) = delete;
    MaybeUnreplicatedPreImageTruncateBlock& operator=(
        const MaybeUnreplicatedPreImageTruncateBlock&) = delete;
    MaybeUnreplicatedPreImageTruncateBlock(MaybeUnreplicatedPreImageTruncateBlock&&) = delete;
    MaybeUnreplicatedPreImageTruncateBlock& operator=(MaybeUnreplicatedPreImageTruncateBlock&&) =
        delete;

private:
    boost::optional<repl::UnreplicatedWritesBlock> _uwb;
};

// Acquires a cursor on the pre-images collection, with a specifiable order (forward / backward).
std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                const CollectionAcquisition& preImagesCollection,
                                                bool forward) {
    const auto rs = preImagesCollection.getCollectionPtr()->getRecordStore();
    return rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), forward);
}

// Returns true if there are no pre-images for given 'uuid'.
bool hasNoPreimages(OperationContext* opCtx,
                    const CollectionAcquisition& preImagesCollection,
                    const UUID& uuid) {
    auto cursor = getCursor(opCtx, preImagesCollection, true /*forward*/);
    RecordId start =
        change_stream_pre_image_id_util::getAbsoluteMinPreImageRecordIdBoundForNs(uuid).recordId();

    auto seekedRecord = cursor->seek(start, SeekableRecordCursor::BoundInclusion::kInclude);
    if (!seekedRecord) {
        return true;
    }

    auto seekedRecordUUID =
        change_stream_pre_image_id_util::getPreImageNsUUID(seekedRecord->data.toBson());
    return seekedRecordUUID != uuid;
}

// Acquires the pre-images collection given 'nsOrUUID'. When provided a UUID, throws
// NamespaceNotFound if the collection is dropped.
auto acquirePreImagesCollectionForRead(OperationContext* opCtx, const UUID& uuid) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(
            NamespaceStringOrUUID{NamespaceString::kChangeStreamPreImagesNamespace.dbName(), uuid},
            PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
            repl::ReadConcernArgs::get(opCtx),
            AcquisitionPrerequisites::kRead),
        MODE_IS);
}

auto acquirePreImagesCollectionForWrite(OperationContext* opCtx, const UUID& uuid) {
    AcquisitionPrerequisites::OperationType acquisitionPrerequisites =
        change_stream_pre_image_util::shouldUseReplicatedTruncatesForPreImages(opCtx)
        ? AcquisitionPrerequisites::kWrite
        : AcquisitionPrerequisites::kUnreplicatedWrite;

    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(
            NamespaceStringOrUUID{NamespaceString::kChangeStreamPreImagesNamespace.dbName(), uuid},
            PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
            repl::ReadConcernArgs::get(opCtx),
            acquisitionPrerequisites),
        MODE_IX);
}

// Performs a ranged truncate over each expired marker in 'truncateMarkersForNsUUID'. Updates the
// "Output" parameters to communicate the respective docs deleted, bytes deleted, and and maximum
// wall time of documents deleted to the caller.
void truncateExpiredMarkersForNsUUID(
    OperationContext* opCtx,
    std::shared_ptr<PreImagesTruncateMarkersPerNsUUID> truncateMarkersForNsUUID,
    const CollectionPtr& preImagesColl,
    const UUID& nsUUID,
    const RecordId& minRecordIdForNs,
    const Timestamp& maxTSEligibleForTruncate,
    bool useReplicatedTruncates,
    int64_t& totalDocsDeletedOutput,
    int64_t& totalBytesDeletedOutput,
    Date_t& maxWallTimeForNsTruncateOutput) {
    while (auto marker = truncateMarkersForNsUUID->peekOldestMarkerIfNeeded(opCtx)) {
        if (change_stream_pre_image_id_util::getPreImageTimestamp(marker->lastRecord) >
            maxTSEligibleForTruncate) {
            // The truncate marker contains pre-images part of a data range not yet consistent
            // (i.e. there could be oplog holes or partially applied ranges of the oplog in the
            // range).
            return;
        }

        writeConflictRetry(opCtx, "truncate pre-images collection", preImagesColl->ns(), [&] {
            auto bytesDeleted = marker->bytes;
            auto docsDeleted = marker->records;

            MaybeUnreplicatedPreImageTruncateBlock mupitb(opCtx, useReplicatedTruncates);
            WriteUnitOfWork wuow(opCtx);
            collection_internal::truncateRange(opCtx,
                                               preImagesColl,
                                               minRecordIdForNs,
                                               marker->lastRecord,
                                               bytesDeleted,
                                               docsDeleted);
            wuow.commit();

            if (marker->wallTime > maxWallTimeForNsTruncateOutput) {
                maxWallTimeForNsTruncateOutput = marker->wallTime;
            }

            truncateMarkersForNsUUID->popOldestMarker();

            totalDocsDeletedOutput += docsDeleted;
            totalBytesDeletedOutput += bytesDeleted;
        });
    }
}

std::unique_ptr<pre_image_marker_initialization_internal::SamplingStrategy> makeSamplingStrategy(
    OperationContext* opCtx) {
    using namespace pre_image_marker_initialization_internal;

    auto numSamplesPerMarker =
        static_cast<uint64_t>(gChangeStreamPreImagesSamplePointsPerUUID.loadRelaxed());
    const auto minBytesPerMarker = gPreImagesCollectionTruncateMarkersMinBytes;

    // On DSC, the 'SizeStorer' that provides the number of records and the size of the data is not
    // yet implemented and will always return 0 for both. Therefore the code below would always
    // choose the "scanning" method for the preimages collection on DSC, which could have a
    // prohibitive cost for large collections. To avoid this, branch to a different approach for
    // sampling the preimages collection if needed. We may want to remove this branch once the
    // 'SizeStorer' becomes available on DSC as well.
    if (change_stream_pre_image_util::shouldUseReplicatedTruncatesForPreImages(opCtx)) {
        return std::make_unique<EqualStepSamplingStrategy>(numSamplesPerMarker, minBytesPerMarker);
    }
    return std::make_unique<PrimaryWithFallbackSamplingStrategy>(
        std::make_unique<RandomSamplingStrategy>(numSamplesPerMarker, minBytesPerMarker),
        std::make_unique<ScanningSamplingStrategy>(minBytesPerMarker));
}
}  // namespace

PreImagesTruncateMarkers::PreImagesTruncateMarkers(OperationContext* opCtx,
                                                   const CollectionAcquisition& preImagesCollection)
    : _preImagesCollectionUUID{preImagesCollection.uuid()} {
    invariant(opCtx != nullptr);
    invariant(preImagesCollection.exists());

    auto samplingStrategy = makeSamplingStrategy(opCtx);
    tassert(11423600,
            "Truncation marker sampling must succeed",
            samplingStrategy->performSampling(opCtx, preImagesCollection, _markersMap));
}

void PreImagesTruncateMarkers::refreshMarkers(OperationContext* opCtx) {
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    // Use writeConflictRetry since acquiring the collection can yield a WriteConflictException if
    // it races with concurrent catalog changes.
    writeConflictRetry(opCtx,
                       "Refreshing the pre image truncate markers in a new snapshot",
                       NamespaceString::kChangeStreamPreImagesNamespace,
                       [&] {
                           // writeConflictRetry automatically abandon's the snapshot before
                           // retrying.
                           //
                           const auto preImagesCollection =
                               acquirePreImagesCollectionForRead(opCtx, _preImagesCollectionUUID);

                           const auto nsUUIDs =
                               change_stream_pre_image_util::getNsUUIDs(opCtx, preImagesCollection);
                           for (const auto& nsUUID : nsUUIDs) {
                               // Account for records inserted into an 'nsUUID' not tracked during
                               // the initial construction of the markers.
                               auto nsUUIDMarkers = _markersMap.getOrEmplace(
                                   nsUUID,
                                   nsUUID,
                                   PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers{},
                                   gPreImagesCollectionTruncateMarkersMinBytes);
                               nsUUIDMarkers->refreshHighestTrackedRecord(opCtx,
                                                                          preImagesCollection);
                           }
                       });
}

int64_t PreImagesTruncateMarkers::getNumberOfSampledCollections() const {
    return static_cast<int64_t>(_markersMap.getUnderlyingSnapshot()->size());
}

PreImagesTruncateStats PreImagesTruncateMarkers::truncateExpiredPreImages(
    OperationContext* opCtx, bool useReplicatedTruncates) {
    const auto markersMapSnapshot = _markersMap.getUnderlyingSnapshot();

    // Truncates are untimestamped. Allow multiple truncates to occur.
    shard_role_details::getRecoveryUnit(opCtx)->allowAllUntimestampedWrites();

    // Acquire locks before iterating the truncate markers to prevent repeated locking and unlocking
    // for each truncate. By making each call to truncate individually retriable, we reduce the
    // amount of book keeping necessary to rollback truncate marker modifications after a
    // WriteConflictException.
    //
    // There are 2 assumptions which make it safe to hold locks in the current scope.
    //      (1) Since ticket acquisition is bypassed, we don't contribute to ticket exhaustion by
    //      wrapping each truncate in it's own 'writeConflictRetry()' (see SERVER-65418 for more
    //      details).
    //      (2) The locks will never be yielded by a query, thus there can't be any concurrent DDL
    //      operations to invalidate our collection instance. This is only a risk when
    //      'abandonSnapshot()' is called, which can invalidate the acquired collection instance,
    //      like after a WriteConflictException.
    const auto preImagesCollection =
        acquirePreImagesCollectionForWrite(opCtx, _preImagesCollectionUUID);
    const auto& preImagesColl = preImagesCollection.getCollectionPtr();

    PreImagesTruncateStats stats;

    // All pre-images with 'ts' <= 'maxTSEligibleForTruncate' are candidates for truncation.
    // However, pre-images with 'ts' > 'maxTSEligibleForTruncate' are unsafe to truncate, as there
    // may be oplog holes or inconsistent data prior to it. Compute the value once, as it requires
    // making an additional call into the storage engine.
    Timestamp maxTSEligibleForTruncate =
        change_stream_pre_image_id_util::getMaxTSEligibleForTruncate(opCtx);
    stats.maxTimestampEligibleForTruncate = maxTSEligibleForTruncate;

    // Truncate markers can be generated with data that is later rolled back via rollback-to-stable.
    // This behavior is acceptable given the following:
    //      (1) Only expired data is truncated (expire by seconds or oldest oplog TS).
    //      (2) If a marker's 'lastRecord' is rolled back, it's wallTime or ts field will eventually
    //      expire. An expired marker's 'lastRecord' serves as an upper bound for the truncate
    //      range. Even if the 'lastRecord' doesn't exist anymore, all pre-images older than it are
    //      truncated for the nsUUID.
    //          . Caveat: Size metadata isn't accurate if pre-image inserts are rolled back. It will
    //          eventually converge to a correct state in absence of another rollback-to-stable.
    //      (3) If a truncate is issued on data that is later rolled back, unexpired pre-images will
    //      be rolled back in the process. From the stable timestamp, oplog entries will be replayed
    //      and re-inserted into truncate markers (mirroring truncate behavior in a stable state).
    for (auto& [nsUUID, truncateMarkersForNsUUID] : *markersMapSnapshot) {
        RecordId minRecordId =
            change_stream_pre_image_id_util::getAbsoluteMinPreImageRecordIdBoundForNs(nsUUID)
                .recordId();

        int64_t docsDeletedForNs = 0;
        int64_t bytesDeletedForNs = 0;
        Date_t maxWallTimeForNsTruncate{};
        truncateExpiredMarkersForNsUUID(opCtx,
                                        truncateMarkersForNsUUID,
                                        preImagesColl,
                                        nsUUID,
                                        minRecordId,
                                        maxTSEligibleForTruncate,
                                        useReplicatedTruncates,
                                        docsDeletedForNs,
                                        bytesDeletedForNs,
                                        maxWallTimeForNsTruncate);

        // Best effort for removing all expired pre-images from 'nsUUID'. If there is a partial
        // marker which can be made into an expired marker, try to remove the new marker as
        // well.
        truncateMarkersForNsUUID->createPartialMarkerIfNecessary(opCtx);
        truncateExpiredMarkersForNsUUID(opCtx,
                                        truncateMarkersForNsUUID,
                                        preImagesColl,
                                        nsUUID,
                                        minRecordId,
                                        maxTSEligibleForTruncate,
                                        useReplicatedTruncates,
                                        docsDeletedForNs,
                                        bytesDeletedForNs,
                                        maxWallTimeForNsTruncate);

        if (maxWallTimeForNsTruncate > stats.maxStartWallTime) {
            stats.maxStartWallTime = maxWallTimeForNsTruncate;
        }
        stats.docsDeleted = stats.docsDeleted + docsDeletedForNs;
        stats.bytesDeleted = stats.bytesDeleted + bytesDeletedForNs;
        stats.scannedInternalCollections++;

        // If the source collection doesn't exist and there's no more data to erase we can safely
        // remove the markers. Perform a final truncate to remove all elements just in case.
        if (CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, nsUUID) == nullptr &&
            truncateMarkersForNsUUID->isEmpty()) {

            // Truncate all pre-images for the given collection, while using the largest timestamp
            // that is still eligible for truncation.
            RecordId maxRecordId =
                change_stream_pre_image_id_util::getPreImageRecordIdForNsTimestampApplyOpsIndex(
                    nsUUID, maxTSEligibleForTruncate, std::numeric_limits<int64_t>::max())
                    .recordId();

            writeConflictRetry(opCtx, "final truncate", preImagesColl->ns(), [&] {
                MaybeUnreplicatedPreImageTruncateBlock mupitb(opCtx, useReplicatedTruncates);
                WriteUnitOfWork wuow(opCtx);
                collection_internal::truncateRange(
                    opCtx, preImagesColl, minRecordId, maxRecordId, 0, 0);
                wuow.commit();
            });

            // All pre-images for the dropped collection must be deleted by this point, as
            // collection drop must appear before 'maxTSEligibleForTruncate'. However, we introduce
            // additional dassert() to ensure we erase the marker only when all preimages are gone.
            dassert(hasNoPreimages(opCtx, preImagesCollection, nsUUID));
            _markersMap.erase(nsUUID);
        }
    }

    return stats;
}

void PreImagesTruncateMarkers::updateOnInsert(const RecordId& recordId,
                                              const UUID& nsUUID,
                                              Date_t wallTime,
                                              int64_t bytesInserted,
                                              int64_t numRecords) {
    auto nsUUIDMarkers = _markersMap.find(nsUUID);
    if (!nsUUIDMarkers) {
        nsUUIDMarkers =
            _markersMap.getOrEmplace(nsUUID,
                                     nsUUID,
                                     PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers{},
                                     gPreImagesCollectionTruncateMarkersMinBytes);
    }
    nsUUIDMarkers->updateMarkers(bytesInserted, recordId, wallTime, numRecords);
}
}  // namespace mongo
