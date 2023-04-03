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

#include <algorithm>
#include <iostream>

#include "mongo/db/pipeline/percentile_algo_tdigest.h"

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class TDigestDistributedClassic : public TDigest, public PartialPercentile<Value> {
    constexpr static size_t kNegInfCountOffset = 0;
    constexpr static size_t kPosInfCountOffset = 1;
    constexpr static size_t kMinOffset = 2;
    constexpr static size_t kMaxOffset = 3;
    constexpr static size_t kCentroidsOffset = 4;

public:
    TDigestDistributedClassic(ScalingFunction k_limit, int delta) : TDigest(k_limit, delta) {}

    /**
     * Serialize the digest's state into a flat array of doubles:
     * [negInfCount, posInfCount, min, max, c0.weight, c0.mean, c1.weight, c1.weight, ...]
     */
    Value serialize() final {
        flushBuffer();

        std::vector<double> flattened(kCentroidsOffset, 0);
        flattened[kNegInfCountOffset] = _negInfCount;
        flattened[kPosInfCountOffset] = _posInfCount;
        flattened[kMinOffset] = _min;
        flattened[kMaxOffset] = _max;

        flattened.reserve(kCentroidsOffset + 2 * _centroids.size());
        for (const Centroid& c : _centroids) {
            flattened.push_back(c.weight);
            flattened.push_back(c.mean);
        }
        return Value(std::vector<Value>(flattened.begin(), flattened.end()));
    }

    /*
     * 'partial' is expected to be an array, created by the 'serialize()' above.
     */
    void combine(const Value& partial) final {
        tassert(7492700,
                "TDigest should have been serialized into an array of even size",
                partial.isArray() && (partial.getArrayLength() % 2 == 0));

        if (partial.getArrayLength() == 0) {
            return;  // the other t-digest was empty, nothing to do here
        }

        tassert(7492701,
                "Serialized array of non-empty TDigest must contain the min of required elements",
                partial.getArrayLength() >= kCentroidsOffset);

        const auto& other = partial.getArray();
        const int64_t negInfCount = other[kNegInfCountOffset].coerceToLong();
        const int64_t posInfCount = other[kPosInfCountOffset].coerceToLong();
        const double min = other[kMinOffset].coerceToDouble();
        const double max = other[kMaxOffset].coerceToDouble();

        std::vector<Centroid> centroids;
        centroids.reserve((other.size() - kCentroidsOffset) / 2);
        for (size_t i = kCentroidsOffset; i < other.size(); i += 2) {
            centroids.push_back({other[i].coerceToDouble(), other[i + 1].coerceToDouble()});
        }

        merge(TDigest{negInfCount, posInfCount, min, max, centroids, _k_limit, _delta});
    }
};

std::unique_ptr<PercentileAlgorithm> createTDigestDistributedClassic() {
    return std::make_unique<TDigestDistributedClassic>(TDigest::k2_limit,
                                                       internalQueryTdigestDelta.load());
}

}  // namespace mongo
