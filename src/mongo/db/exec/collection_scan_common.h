// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/record_id.h"
#include "mongo/util/modules.h"

namespace mongo {

struct ResumeScanPoint {
    RecordId recordId;
    bool tolerateKeyNotFound = false;
};

struct [[MONGO_MOD_PUBLIC]] CollectionScanParams {
    enum Direction {
        FORWARD = 1,
        BACKWARD = -1,
    };

    enum class ScanBoundInclusion {
        kExcludeBothStartAndEndRecords = 0b00,
        kIncludeStartRecordOnly = 0b01,
        kIncludeEndRecordOnly = 0b10,
        kIncludeBothStartAndEndRecords = 0b11,
    };

    static ScanBoundInclusion makeInclusion(bool startInclusive, bool endInclusive) {
        return ScanBoundInclusion(int(startInclusive) | (int(endInclusive) << 1));
    }

    // If present, this parameter sets the start point of a forward scan or the end point of a
    // reverse scan. A forward scan will start scanning at the document with the lowest RecordId
    // greater than or equal to minRecord. A reverse scan will stop and return EOF on the first
    // document with a RecordId less than minRecord, or a higher record if none exists. May only
    // be used for scans on clustered collections and forward oplog scans. If exclusive
    // bounds are required, a MatchExpression must be passed to the CollectionScan stage. This field
    // cannot be used in conjunction with 'resumeScanPoint'.
    boost::optional<RecordIdBound> minRecord;

    // If present, this parameter sets the start point of a reverse scan or the end point of a
    // forward scan. A forward scan will stop and return EOF on the first document with a RecordId
    // greater than maxRecord. A reverse scan will start scanning at the document with the
    // highest RecordId less than or equal to maxRecord, or a lower record if none exists. May
    // only be used for scans on clustered collections and forward oplog scans. If exclusive
    // bounds are required, a MatchExpression must be passed to the CollectionScan stage. This field
    // cannot be used in conjunction with 'resumeScanPoint'.
    boost::optional<RecordIdBound> maxRecord;

    // If true, the collection scan will return a token that can be used to resume the scan.
    bool requestResumeToken = false;

    // If present, collection scan will seek to the exact RecordId.
    // - If 'tolerateKeyNotFound' is false, and if the RecordId does not exist, it will raise
    // KeyNotFound.
    // - If 'tolerateKeyNotFound' is true, and if the RecordId does not exist, it will seek to the
    // next valid one.
    // This field must only be set on forward collection scans and cannot be used in conjunction
    // with 'minRecord' or 'maxRecord'.
    boost::optional<ResumeScanPoint> resumeScanPoint;

    Direction direction = FORWARD;

    // By default, both start and end records will be included.
    //
    // For a FORWARD scan, the startRecord is the minRecord. For a BACKWARD scan, the startRecord is
    // the maxRecord.
    //
    // Only compatible with bounded collection scans. Only excludes record bounds if the bound is
    // also defined.
    // Ex) A forward scan with [minRecord, maxRecord] of [boost::none, 10] and
    // ScanBoundInclusion::kIncludeEndRecordOnly will yield the same results as
    // ScanBoundInclusion::kIncludeBothStartAndEndRecords since the startRecord that would have been
    // excluded is not defined anyway.
    //
    // Use with caution, as this can override a filter.
    // Ex) Suppose we have [minRecord, maxRecord] of [-10, 10],
    // ScanBoundInclusion::kIncludeEndRecordOnly, and a filter {$gte: RecordId(-10)} for a forward
    // scan. The results would still exclude RecordId(-10) due to the ScanBoundInclusion.
    ScanBoundInclusion boundInclusion = ScanBoundInclusion::kIncludeBothStartAndEndRecords;

    // Do we want the scan to be 'tailable'?  Only meaningful if the collection is capped.
    bool tailable = false;

    // Assert that the specified timestamp has not fallen off the oplog on a forward scan.
    boost::optional<Timestamp> assertTsHasNotFallenOff = boost::none;

    // Should we keep track of the timestamp of the latest oplog entry we've seen? This information
    // is needed to merge cursors from the oplog in order of operation time when reading the oplog
    // across a sharded cluster.
    bool shouldTrackLatestOplogTimestamp = false;

    // Once the first matching document is found, assume that all documents after it must match.
    // This is useful for oplog queries where we know we will see records ordered by the ts field.
    bool stopApplyingFilterAfterFirstMatch = false;

    // Whether or not to wait for oplog visibility on oplog collection scans.
    bool shouldWaitForOplogVisibility = false;

    // Whether or not to return EOF and stop further scanning once MatchExpression evaluates to
    // false. Can only be set to true if the MatchExpression is present.
    bool shouldReturnEofOnFilterMismatch = false;
};

}  // namespace mongo
