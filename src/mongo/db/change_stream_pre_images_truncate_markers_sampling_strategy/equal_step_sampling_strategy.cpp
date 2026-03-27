/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/equal_step_sampling_strategy.h"

#include "mongo/db/change_stream_pre_image_id_util.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::pre_image_marker_initialization_internal {
namespace {
// Sample the data range of the pre-images collection for the given 'nsUUID' value between the
// minimum established record to the maximum established record, in approximately equal-sized steps.
// The minimum/maximum records need to be passed in as 'firstRidAndWall' and 'lastRidAndWall'. These
// bounds are exclusive.
// Additional samples are added to the 'samples' out parameter, if they exist.
// At most 'numSamples' additional samples will be produced. 'numSamples' has to be at least 1 when
// calling this function.
void sampleRangeEquallyWithCursor(SeekableRecordCursor& cursor,
                                  const UUID& nsUUID,
                                  const RecordIdAndWallTime& firstRidAndWall,
                                  const RecordIdAndWallTime& lastRidAndWall,
                                  uint64_t numSamples,
                                  std::vector<RecordIdAndWallTime>& samples) {
    // Sample size needs at least 1.
    invariant(numSamples >= 1);

    // Convert lowest and highest values into uint128_t values, for easy arithmetic.
    const auto lowest =
        change_stream_pre_image_id_util::timestampAndApplyOpsIndexToNumber(firstRidAndWall.id);
    const auto highest =
        change_stream_pre_image_id_util::timestampAndApplyOpsIndexToNumber(lastRidAndWall.id);
    invariant(lowest <= highest);

    const auto distance = highest - lowest;

    // Clamp 'stepSize' to at least 1, so that the algorithm is guaranteed to make progress. Note
    // that the step size is calculated using integer division, so that the 'stepSize' value is
    // rounded down. Adding 'stepSize' multiple times can lead to a cumulative rounding error. This
    // should be negligible for real-word use cases though, as records should mostly have Timestamp
    // differences, and the Timestamp part accounts for the upper 64 bits of the distance.
    const auto stepSize = std::max<decltype(distance)>(distance / numSamples, 1);

    // Add 'stepSize' here and start sampling at 'lowest + stepSize' because we have already
    // included the lowest possible record.
    auto current = lowest + stepSize;

    while (current < highest && samples.size() < numSamples) {
        auto [currentTs, currentApplyOpsIndex] =
            change_stream_pre_image_id_util::timestampAndApplyOpsIndexFromNumber(current);

        RecordId seekTo =
            change_stream_pre_image_id_util::getPreImageRecordIdForNsTimestampApplyOpsIndex(
                nsUUID, currentTs /* timestamp */, currentApplyOpsIndex /* applyOpsIndex */)
                .recordId();

        boost::optional<Record> record =
            cursor.seek(seekTo, SeekableRecordCursor::BoundInclusion::kInclude);

        if (!record) {
            // No record found, so we are at the end of the collection already. Abort
            // sampling. We should not get here, because we should see the record with the highest
            // Timestamp for the 'nsUUID' before. However, this is left as a safeguard so there is
            // no dereferencing of empty optional values.
            dassert(false);
            return;
        }

        if (record->id == lastRidAndWall.id) {
            // We have reached the maximum record for this 'nsUUID' already. The maximum
            // record is added after the loop separately.
            return;
        }

        const BSONObj preImageObj = record->data.toBson();

        if (nsUUID != change_stream_pre_image_id_util::getPreImageNsUUID(preImageObj)) {
            // Record is already for a different 'nsUUID' value. Abort sampling.
            // We should not get here, because the current record id compared different to the
            // maximum record id we expect for the 'nsUUID' value. However, this is left as a
            // safeguard so samples for an unexpected 'nsUUID' will never be considered.
            dassert(false);
            return;
        }

        // Add sample.
        samples.emplace_back(record->id,
                             PreImagesTruncateMarkersPerNsUUID::getWallTime(preImageObj));

        // Forward to next Timestamp value. If the timestamp part of the record just read is already
        // larger than the Timestamp for the next planned step, we can as well bump it up to what we
        // just read plus the step size. That way we can avoid reading the same records again if
        // they are farther apart than the step size.
        current = std::max(current,
                           change_stream_pre_image_id_util::timestampAndApplyOpsIndexToNumber(
                               record->id)) +
            stepSize;
    }
}
}  // namespace

std::vector<RecordIdAndWallTime> EqualStepSamplingStrategy::sampleNSUUIDRangeEqually(
    OperationContext* opCtx,
    const CollectionAcquisition& preImagesCollection,
    UUID nsUUID,
    const RecordIdAndWallTime& lastRidAndWall,
    uint64_t numSamples) {
    tassert(11423700, "Expecting numSamplesPerMarker to be at least 1", numSamples > 0);

    std::vector<RecordIdAndWallTime> samples;

    if (numSamples >= 2) {
        // We already have the sample value for the highest Timestamp value for this 'nsUUID' value.
        // If more samples were requested, open an additional cursor and scan the 'nsUUID' range in
        // approximately equal-sized steps.
        const auto rs = preImagesCollection.getCollectionPtr()->getRecordStore();
        auto cursor =
            rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), true /*forward*/);

        // Seek to lowest possible entry for this 'nsUUID' value, i.e. the one with timestamp 0 and
        // applyOpsIndex 0.
        RecordId seekTo =
            change_stream_pre_image_id_util::getAbsoluteMinPreImageRecordIdBoundForNs(nsUUID)
                .recordId();

        boost::optional<Record> record =
            cursor->seek(seekTo, SeekableRecordCursor::BoundInclusion::kInclude);

        // Expect to see at least one record here for the 'nsUUID' value, as we previously have
        // gotten an entry for this 'nsUUID' value.
        invariant(record.has_value());
        const auto firstRidAndWall =
            PreImagesTruncateMarkersPerNsUUID::getRecordIdAndWallTime(*record);
        const BSONObj preImageObj = record->data.toBson();
        invariant(nsUUID == change_stream_pre_image_id_util::getPreImageNsUUID(preImageObj));

        if (firstRidAndWall.id != lastRidAndWall.id) {
            // The record we just read is not identical to the record with the highest Timestamp for
            // this 'nsUUID' value, which was already tracked it while sampling the distinct
            // 'nsUUID' values. We can expect more records to be present and thus want to sample
            // them.
            // The record we just read is different to the record with the highest rid and wall
            // time. Track it and calculate the steps for sampling.
            samples.push_back(firstRidAndWall);

            sampleRangeEquallyWithCursor(
                *cursor,
                nsUUID,
                firstRidAndWall,
                lastRidAndWall,
                // Subtract 1 here because one sample was already added above.
                numSamples - 1,
                samples);
        }
    }

    // Finally push the sample for the highest Timestamp that was gathered initially.
    samples.push_back(lastRidAndWall);

    LOGV2_DEBUG(
        11423707,
        5,
        "Timestamp samples produced by equal-step sampling",
        "nsUUID"_attr = nsUUID,
        "numSamples"_attr = numSamples,
        "samples"_attr = [&]() {
            std::vector<Timestamp> ret;
            for (const auto& s : samples) {
                ret.push_back(change_stream_pre_image_id_util::getPreImageTimestamp(s.id));
            }
            return ret;
        }());

    invariant(samples.size() > 0);
    invariant(samples.size() <= numSamples);

    // Ensure sortedness of the entries in 'samples' for this 'nsUUID' value.
    dassert(std::is_sorted(
        samples.begin(),
        samples.end(),
        [](const RecordIdAndWallTime& a, const RecordIdAndWallTime& b) { return a.id < b.id; }));

    return samples;
}

bool EqualStepSamplingStrategy::performSampling(OperationContext* opCtx,
                                                const CollectionAcquisition& preImagesCollection,
                                                MarkersMap& markersMap) {
    tassert(11423701, "Expecting numSamplesPerMarker to be at least 1", _numSamplesPerMarker > 0);

    LOGV2_INFO(11423704,
               "Choosing equal step pre-images collection sampling",
               "numSamplesPerMarker"_attr = _numSamplesPerMarker,
               "preImagesCollectionUUID"_attr = preImagesCollection.uuid());

    // Extract the maximum timestamps for each 'nsUUID' value in the preimages collection by
    // performing a loose reverse scan.
    const auto nsUUIDLastRecords = sampleLastRecordPerNsUUID(opCtx, preImagesCollection);

    if (nsUUIDLastRecords.empty()) {
        // Nothing to do if there are no preimages for _any_ nsUUIDs.
        return true;
    }

    // Variables used for progress reporting.
    Timer lastProgressTimer;
    const auto samplingLogIntervalSeconds = gCollectionSamplingLogIntervalSeconds.load();
    int i = 0;

    // The actual samples gathered, per distinct 'nsUUID' value.
    SamplesMap samples;

    for (const auto& [nsUUID, lastRidAndWall] : nsUUIDLastRecords) {
        // Sample the collection for the current 'nsUUID' value in approximately equal steps.
        // Samples will be added to the 'samples' vector for the current 'nsUUID' value in
        // ascending timestamp order.
        samples.emplace(
            nsUUID,
            sampleNSUUIDRangeEqually(
                opCtx, preImagesCollection, nsUUID, lastRidAndWall, _numSamplesPerMarker));

        // Progress reporting.
        if (samplingLogIntervalSeconds > 0 &&
            lastProgressTimer.elapsed() >= Seconds(samplingLogIntervalSeconds)) {
            LOGV2(11423706,
                  "Pre-images collection equal-step sampling progress",
                  "namespace"_attr = preImagesCollection.nss(),
                  "preImagesCollectionUUID"_attr = preImagesCollection.uuid(),
                  "nsUUIDs"_attr = nsUUIDLastRecords.size(),
                  "numSamplesPerMarker"_attr = _numSamplesPerMarker,
                  "randomSamplesCompleted"_attr = (i + 1));
            lastProgressTimer.reset();
        }
        i++;
    }

    tassert(11423702,
            "Expecting number of sample keys to equal number of last records keys",
            samples.size() == nsUUIDLastRecords.size());

    // We cannot rely on the 'SizeStorer' here, as it may return 0 for both 'numRecords' and
    // 'dataSize' with disaggregated storage. Specifically, replicated fastcount information is not
    // available for implicitly replicated collections such as 'config.system.preimages'. That means
    // we need to make up estimates for 'numRecords' and 'dataSize' here:
    // - 'numRecords' is set to the number of pre-images found during the sampling.
    //   This is guaranteed to be at least 1.
    // - 'dataSize' is set to the value of 'numRecords' * 1024.
    // These estimates can be very inaccurate.
    int64_t numRecords = std::max<int64_t>(1, countTotalSamples(samples));

    // Multiply the number of records by an arbitrarily-chosen average record size here to get
    // to a data size estimate.
    int64_t dataSize = numRecords * kRecordSizeEstimate;
    invariant(dataSize > 0);
    const auto minBytesPerMarker = _minBytesPerMarker;
    const double avgRecordSize = static_cast<double>(dataSize) / static_cast<double>(numRecords);
    const double estimatedRecordsPerMarker = std::ceil(minBytesPerMarker / avgRecordSize);
    const double estimatedBytesPerMarker = estimatedRecordsPerMarker * avgRecordSize;

    LOGV2_DEBUG(
        11423705,
        3,
        "Statistics after equal step pre-images collection sampling. These are partially based on "
        "estimates and heuristics, so they can be inaccurate.",
        "nsUUIDs"_attr = nsUUIDLastRecords.size(),
        "numSamplesPerMarker"_attr = _numSamplesPerMarker,
        "dataSizeSampled"_attr = dataSize,
        "numRecordsSampled"_attr = numRecords,
        "avgRecordSizeSampled"_attr = avgRecordSize,
        "estimatedRecordsPerMarker"_attr = estimatedRecordsPerMarker,
        "estimatedBytesPerMarker"_attr = estimatedBytesPerMarker,
        "minBytesPerMarker"_attr = minBytesPerMarker,
        "preImagesCollectionUUID"_attr = preImagesCollection.uuid());

    // Use the samples to generate and install in 'markersMap' the initial sets of whole markers
    // for each distinct 'nsUUID' value. And then fix the install entries in 'markersMap' so the
    // aggregate number of records and bytes across the map make up the expected number of
    // records and bytes in the pre-images collection. Note that we use a value of 1 here for
    // 'randomSamplesPerMarker', because this will guarantee that all samples we have gathered
    // will lead to the establishment of a "whole" collection truncate marker. Passing in the
    // actual value of 'randomSamplesPerMarker' could lead to no whole collection truncate
    // markers to be created, which would inhibit immediate preimage deletion for the oldest,
    // already expired entries.
    installMarkersInMapFromSamples(opCtx,
                                   preImagesCollection,
                                   samples,
                                   numRecords,
                                   dataSize,
                                   minBytesPerMarker,
                                   1 /* randomSamplesPerMarker */,
                                   estimatedRecordsPerMarker,
                                   estimatedBytesPerMarker,
                                   markersMap);
    return true;
}

}  // namespace mongo::pre_image_marker_initialization_internal
