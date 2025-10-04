/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/mirroring_sampler.h"

#include "mongo/db/mirror_maestro_feature_flags_gen.h"
#include "mongo/util/assert_util.h"

#include <chrono>
#include <cmath>
#include <utility>

namespace mongo {

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

    if (params.value < static_cast<int>(params.max * params.targetedRatio)) {
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
