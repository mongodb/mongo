// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/percentile_algo.h"
#include "mongo/db/pipeline/percentile_algo_tdigest.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

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
