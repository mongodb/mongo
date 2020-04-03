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

#include <cmath>
#include <cstdlib>

#include "mongo/db/mirroring_sampler.h"

namespace mongo {

MirroringSampler::SamplingParameters::SamplingParameters(const double ratio_,
                                                         const int rndMax,
                                                         const int rndValue)
    : ratio{ratio_}, max{rndMax}, value{rndValue} {

    invariant(ratio <= 1.0);
    invariant(ratio >= 0.0);

    invariant(value <= max);
    invariant(value >= 0);
}

MirroringSampler::SamplingParameters::SamplingParameters(const double ratio,
                                                         const int rndMax,
                                                         RandomFunc rnd)
    : SamplingParameters(ratio, rndMax, [&] {
          if (ratio == 0.0) {
              // We should never sample, avoid invoking rnd().
              return rndMax;
          }

          if (ratio == 1.0) {
              // We should always sample, avoid invoking rnd().
              return 0;
          }

          return std::move(rnd)();
      }()) {}

bool MirroringSampler::shouldSample(const std::shared_ptr<const repl::IsMasterResponse>& imr,
                                    const SamplingParameters& params) const noexcept {
    if (!imr) {
        // If we don't have an IsMasterResponse, we can't know where to send our mirrored request.
        return false;
    }

    const auto secondariesCount = imr->getHosts().size() - 1;
    if (!imr->isMaster() || secondariesCount < 1) {
        // If this is not the primary, or there are no eligible secondaries, nothing more to do.
        return false;
    }
    invariant(secondariesCount > 0);

    // Adjust ratio to mirror read requests to approximately `samplingRate x secondariesCount`.
    const auto secondariesRatio = secondariesCount * params.ratio;
    const auto mirroringFactor = std::ceil(secondariesRatio);
    invariant(mirroringFactor > 0 && mirroringFactor <= secondariesCount);
    const double adjustedRatio = secondariesRatio / mirroringFactor;

    // If our value is less than our max, then take a sample.
    return params.value < static_cast<int>(params.max * adjustedRatio);
}

std::vector<HostAndPort> MirroringSampler::getRawMirroringTargets(
    const std::shared_ptr<const repl::IsMasterResponse>& isMaster) noexcept {
    invariant(isMaster);
    if (!isMaster->isMaster()) {
        // Don't mirror if we're not primary
        return {};
    }

    const auto& hosts = isMaster->getHosts();
    if (hosts.size() < 2) {
        // Don't mirror if we're standalone
        return {};
    }

    const auto& self = isMaster->getPrimary();

    std::vector<HostAndPort> potentialTargets;
    for (auto& host : hosts) {
        if (host != self) {
            potentialTargets.push_back(host);
        }
    }

    return potentialTargets;
}

std::vector<HostAndPort> MirroringSampler::getMirroringTargets(
    const std::shared_ptr<const repl::IsMasterResponse>& isMaster,
    const double ratio,
    RandomFunc rnd,
    const int rndMax) noexcept {

    auto sampler = MirroringSampler();

    auto samplingParams = SamplingParameters(ratio, rndMax, std::move(rnd));
    if (!sampler.shouldSample(isMaster, samplingParams)) {
        return {};
    }

    return sampler.getRawMirroringTargets(isMaster);
}

}  // namespace mongo
