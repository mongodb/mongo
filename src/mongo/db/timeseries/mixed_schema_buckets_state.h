// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

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
