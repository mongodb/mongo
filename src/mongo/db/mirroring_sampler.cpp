// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/mirroring_sampler.h"

#include "mongo/db/mirror_maestro_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <chrono>
#include <cmath>
#include <utility>

namespace mongo {

MONGO_FAIL_POINT_DEFINE(disableTargetedMirroring);

MirroringSampler::SamplingParameters::SamplingParameters(const double generalRatio_,
                                                         const double targetedRatio_,
                                                         const int rndMax,
                                                         const int rndValue)
    : generalRatio{generalRatio_}, targetedRatio{targetedRatio_}, max{rndMax}, value{rndValue} {

    invariant(generalRatio <= 1.0);
    invariant(generalRatio >= 0.0);

    invariant(targetedRatio <= 1.0);
    invariant(targetedRatio >= 0.0);

    invariant(value <= max);
    invariant(value >= 0);
}

MirroringSampler::SamplingParameters::SamplingParameters(const double generalRatio,
                                                         const double targetedRatio,
                                                         const int rndMax,
                                                         RandomFunc rnd)
    : SamplingParameters(generalRatio, targetedRatio, rndMax, [&] {
          if (generalRatio == 0.0 && targetedRatio == 0.0) {
              // We should never sample, avoid invoking rnd().
              return rndMax;
          }

          if ((generalRatio == 1.0 && (targetedRatio == 0.0 || targetedRatio == 1.0)) ||
              (targetedRatio == 1.0 && (generalRatio == 0.0 || generalRatio == 1.0))) {
              // We should always sample, avoid invoking rnd().
              return 0;
          }

          return std::move(rnd)();
      }()) {}

MirroringSampler::MirroringMode MirroringSampler::getMirrorMode(
    const std::shared_ptr<const repl::HelloResponse>& helloResp,
    const SamplingParameters& params) const {
    MirroringMode mode;

    if (helloResp && params.generalRatio != 0) {
        const int secondariesCount = helloResp->getHosts().size() - 1;
        if (secondariesCount < 1) {
            // There are no eligible nodes to mirror to
            return mode;
        }

        // Adjust ratio to mirror read requests to approximately `samplingRate x secondariesCount`.
        const auto secondariesRatio = secondariesCount * params.generalRatio;
        const auto mirroringFactor = std::ceil(secondariesRatio);
        invariant(mirroringFactor > 0 && mirroringFactor <= secondariesCount);
        const double adjustedRatio = secondariesRatio / mirroringFactor;
        if (helloResp->isWritablePrimary() &&
            (params.value < static_cast<int>(params.max * adjustedRatio))) {
            mode.generalEnabled = true;
        }
    }

    if (!gFeatureFlagTargetedMirrorReads.isEnabled()) {
        return mode;
    }

    if (!disableTargetedMirroring.shouldFail() &&
        params.value < static_cast<int>(params.max * params.targetedRatio)) {
        mode.targetedEnabled = true;
    }

    return mode;
}

std::vector<HostAndPort> MirroringSampler::getRawMirroringTargetsForGeneralMode(
    const std::shared_ptr<const repl::HelloResponse>& helloResp) {
    invariant(helloResp);
    if (!helloResp->isWritablePrimary()) {
        // Don't mirror if we're not primary
        return {};
    }

    const auto& hosts = helloResp->getHosts();
    if (hosts.size() < 2) {
        // Don't mirror if we're standalone
        return {};
    }

    const auto& self = helloResp->getPrimary();

    std::vector<HostAndPort> potentialTargets;
    for (auto& host : hosts) {
        if (host != self) {
            potentialTargets.push_back(host);
        }
    }

    return potentialTargets;
}

std::vector<HostAndPort> MirroringSampler::getGeneralMirroringTargets(
    const std::shared_ptr<const repl::HelloResponse>& helloResp,
    const double generalRatio,
    const double targetedRatio,
    RandomFunc rnd,
    const int rndMax) {

    auto sampler = MirroringSampler();

    auto samplingParams = SamplingParameters(generalRatio, targetedRatio, rndMax, std::move(rnd));
    auto mirrorMode = sampler.getMirrorMode(helloResp, samplingParams);
    if (!mirrorMode.generalEnabled) {
        return {};
    }

    return sampler.getRawMirroringTargetsForGeneralMode(helloResp);
}

}  // namespace mongo
