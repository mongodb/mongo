/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

namespace mongo::timeseries {

/**
 * Indicates the possible presence of mixed-schema buckets according to the collection metadata.
 */
class MixedSchemaBucketsState {
public:
    enum State {
        /**
         * There is no mixed-schema bucket state for this collection.
         * This indicates that either the collection is not time-series,
         * or that is is a time-series collection with invalid catalog metadata.
         */
        Invalid,
        /**
         * There is no indication that time-series collection contains mixed-schema buckets.
         * As such, queries can apply optimizations that assume that no mixed-schema buckets exists.
         * Mixed-schema bucket insertions are rejected, and collection validation fails if it finds
         * mixed-schema buckets.
         */
        NoMixedSchemaBuckets,
        /**
         * This collection is flagged as potentially having mixed-schema buckets.
         * However, its catalog mixed-schema flag is affected by SERVER-91194.
         * As such, queries should assume that mixed-schema buckets may exist.
         * If mixed-schema buckets are inserted, or discovered during collection validation,
         * the user must be directed to the mitigation for SERVER-91194.
         */
        NonDurableMayHaveMixedSchemaBuckets,
        /**
         * This collection is flagged as potentially having mixed-schema buckets,
         * and has already been mitigated for SERVER-91194.
         * As such, queries should assume that mixed-schema buckets may exist,
         * and insertion of mixed-schema buckets is permitted.
         */
        DurableMayHaveMixedSchemaBuckets,
    };

    explicit(false) MixedSchemaBucketsState(State state) : _state(state) {}

    bool isValid() const {
        return _state != Invalid;
    }

    bool mustConsiderMixedSchemaBucketsInReads() const {
        // Intentionally returns true for State::Invalid to fail safe for invalid catalog metadata.
        return _state != NoMixedSchemaBuckets;
    }

    bool canStoreMixedSchemaBucketsSafely() const {
        return _state == DurableMayHaveMixedSchemaBuckets;
    }

private:
    State _state;
};

}  // namespace mongo::timeseries
