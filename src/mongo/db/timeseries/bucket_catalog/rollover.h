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

namespace mongo::timeseries::bucket_catalog {

/**
 * Mode enum to determine the rollover type decision for a given bucket.
 */
enum class RolloverAction {
    kNone,       // Keep bucket open
    kArchive,    // Archive bucket
    kSoftClose,  // Close bucket so it remains eligible for reopening
    kHardClose,  // Permanently close bucket
};

/**
 * Reasons why a bucket was rolled over.
 */
enum class RolloverReason {
    kNone,           // Not actually rolled over
    kTimeForward,    // Measurement time would violate max span for this bucket
    kTimeBackward,   // Measurement time was before bucket min time
    kCount,          // Adding this measurement would violate max count
    kSchemaChange,   // This measurement has a schema incompatible with existing measurements
    kCachePressure,  // System is under cache pressure, and adding this measurement would make
                     // the bucket larger than the dynamic size limit
    kSize,  // Adding this measurement would make the bucket larger than the normal size limit
};

/**
 * Returns the RolloverAction based on the RolloverReason.
 */
RolloverAction getRolloverAction(RolloverReason reason);

}  // namespace mongo::timeseries::bucket_catalog
