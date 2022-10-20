/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/timestamp.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/record_id.h"

namespace mongo {

struct CollectionScanParams {
    enum Direction {
        FORWARD = 1,
        BACKWARD = -1,
    };

    enum class ScanBoundInclusion {
        kExcludeBothStartAndEndRecords,
        kIncludeStartRecordOnly,
        kIncludeEndRecordOnly,
        kIncludeBothStartAndEndRecords,
    };

    // If present, this parameter sets the start point of a forward scan or the end point of a
    // reverse scan. A forward scan will start scanning at the document with the lowest RecordId
    // greater than or equal to minRecord. A reverse scan will stop and return EOF on the first
    // document with a RecordId less than minRecord, or a higher record if none exists. May only
    // be used for scans on clustered collections and forward oplog scans. If exclusive
    // bounds are required, a MatchExpression must be passed to the CollectionScan stage. This field
    // cannot be used in conjunction with 'resumeAfterRecordId'
    boost::optional<RecordIdBound> minRecord;

    // If present, this parameter sets the start point of a reverse scan or the end point of a
    // forward scan. A forward scan will stop and return EOF on the first document with a RecordId
    // greater than maxRecord. A reverse scan will start scanning at the document with the
    // highest RecordId less than or equal to maxRecord, or a lower record if none exists. May
    // only be used for scans on clustered collections and forward oplog scans. If exclusive
    // bounds are required, a MatchExpression must be passed to the CollectionScan stage. This field
    // cannot be used in conjunction with 'resumeAfterRecordId'.
    boost::optional<RecordIdBound> maxRecord;

    // If true, the collection scan will return a token that can be used to resume the scan.
    bool requestResumeToken = false;

    // If present, the collection scan will seek to the exact RecordId, or return KeyNotFound if it
    // does not exist. Must only be set on forward collection scans.
    // This field cannot be used in conjunction with 'minRecord' or 'maxRecord'.
    boost::optional<RecordId> resumeAfterRecordId;

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

    // Should we keep track of the timestamp of the latest oplog or change collection entry we've
    // seen? This information is needed to merge cursors from the oplog in order of operation time
    // when reading the oplog across a sharded cluster.
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
