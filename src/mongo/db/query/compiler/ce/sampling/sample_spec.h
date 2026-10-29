// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <cstddef>

#include <boost/optional/optional.hpp>

namespace mongo {

class QueryKnobConfiguration;

namespace ce {

using CardinalityEstimate = mongo::cost_based_ranker::CardinalityEstimate;

/**
 * Describes a unique sample.
 */
struct SampleSpec {
    // Collection the sample is drawn from.
    NamespaceString nss;
    size_t requestedSampleSize;
    // Technique to use when the sample is generated on the fly.
    SamplingCEMethodEnum onTheFlySamplingMethod;
    // Technique to use when looking up the persisted sample or persisting one.
    SamplingCEMethodEnum persistentSamplingMethod;
    // Required for a chunk sample, ignored otherwise.
    boost::optional<int> numChunks;
    // OTF or persisted. When generating a new persistent sample this must be OTF.
    SamplingSourceEnum samplingSource;
    // Top-level fields to include in the sample. Empty means the whole document is sampled.
    StringSet topLevelSampleFieldNames;
};

/**
 * Converts a SamplingCEMethodEnum value into its equivalent SamplingTechniqueEnum value.
 * TODO SERVER-129240: Remove this helper once types are unified.
 */
inline SamplingTechniqueEnum samplingMethodToTechnique(SamplingCEMethodEnum samplingMethod) {
    switch (samplingMethod) {
        case SamplingCEMethodEnum::kRandom:
            return SamplingTechniqueEnum::kRandom;
        case SamplingCEMethodEnum::kChunk:
            return SamplingTechniqueEnum::kChunk;
        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * The sampling technique a loader for the given spec will use, accounting for test-only overrides
 * and samples that cover the whole collection. 'collectionCard' is the cardinality of the
 * collection the sample is drawn from.
 */
SamplingTechniqueEnum resolveSamplingTechnique(const SampleSpec& spec, size_t collectionCard);

/**
 * Sample size implied by the relevant knobs. Unless overridden, calculated from the confidence
 * interval and margin of error:
 * n = Z^2 / W^2
 * where Z is the z-score for the confidence interval and W is the width of the confidence
 * interval, W = 2 * MoE.
 */
size_t calculateSampleSize(const QueryKnobConfiguration& qkc);

}  // namespace ce
}  // namespace mongo
