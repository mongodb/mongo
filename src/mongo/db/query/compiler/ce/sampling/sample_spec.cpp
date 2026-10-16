// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/sample_spec.h"

#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/util/assert_util.h"

#include <cmath>

namespace mongo::ce {

namespace {
/**
 * Test-only sampling techniques override both the persistent-sample lookup and on-the-fly
 * generation, so that tests can produce repeatable samples.
 */
boost::optional<SamplingTechniqueEnum> getTestOnlySamplingModeIfSet() {
    // internalQuerySamplingBySequentialScan overrides internalQuerySamplingByStrides if both are
    // set
    if (internalQuerySamplingBySequentialScan.load()) {
        // This is only used for testing purposes when a repeatable sample is needed.
        return SamplingTechniqueEnum::kSeqScan;
    } else if (internalQuerySamplingByStrides.load()) {
        // This is only used for testing purposes when a repeatable sample is needed.
        return SamplingTechniqueEnum::kStrides;
    }

    // Neither knob is set, so not in test only sampling mode
    return boost::none;
}

/**
 * Helper function mapping confidence intervals to Z-scores.
 * A "95 confidence interval" means that 95% of the observations (in our case CEs computed from
 * samples) lie within that interval. The interval is defined based on a number of standard
 * deviations(Z-score) from the mean. The Z-score for 95% interval is 1.96, meaning that 95% of the
 * observations lie within 1.96 standard deviations from the mean.
 * https://en.wikipedia.org/wiki/Standard_score
 */
double getZScore(SamplingConfidenceIntervalEnum confidenceInterval) {
    switch (confidenceInterval) {
        case SamplingConfidenceIntervalEnum::k90:
            return 1.645;
        case SamplingConfidenceIntervalEnum::k95:
            return 1.96;
        case SamplingConfidenceIntervalEnum::k99:
            return 2.576;
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace

SamplingTechniqueEnum resolveSamplingTechnique(const SampleSpec& spec, size_t collectionCard) {
    // Test-only knobs override the requested method.
    if (const auto testOnlySamplingMode = getTestOnlySamplingModeIfSet()) {
        return testOnlySamplingMode.value();
    }

    // Persistent samples otherwise always match the method provided by the spec, since that is
    // what they are keyed by on disk. A persisted sample that covers the whole collection is still
    // looked up by its requested method.
    if (spec.samplingSource == SamplingSourceEnum::kPersistentSample) {
        return samplingMethodToTechnique(spec.persistentSamplingMethod);
    }

    // If the requested sample is too large then the whole collection is sampled.
    if (spec.requestedSampleSize >= collectionCard) {
        return SamplingTechniqueEnum::kFullCollScan;
    }

    return samplingMethodToTechnique(spec.onTheFlySamplingMethod);
}

size_t calculateSampleSize(const QueryKnobConfiguration& qkc) {
    const int sampleSizeOverride = qkc.getSamplingSizeOverride();
    if (sampleSizeOverride > 0) {
        return static_cast<size_t>(sampleSizeOverride);
    }

    SamplingConfidenceIntervalEnum ci = qkc.getConfidenceInterval();
    double marginOfError = qkc.getSamplingMarginOfError();
    uassert(9406301, "Margin of error should be larger than 0.", marginOfError > 0);
    double z = getZScore(ci);
    double ciWidth = 2 * marginOfError / 100.0;
    size_t sampleSize = static_cast<size_t>(std::lround((z * z) / (ciWidth * ciWidth)));
    tassert(11301000, "Sample size should be larger than 0.", sampleSize > 0);

    return sampleSize;
}

}  // namespace mongo::ce
