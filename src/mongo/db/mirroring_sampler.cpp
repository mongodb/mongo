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

std::vector<HostAndPort> MirroringSampler::getMirroringTargets(
    std::shared_ptr<const repl::IsMasterResponse> isMaster,
    const double ratio,
    RandomFunc rnd,
    const int rndMax) noexcept {

    invariant(ratio >= 0 && ratio <= 1);
    invariant(isMaster);
    if (!isMaster->isMaster()) {
        return {};
    }

    /**
     * `ratio == 0` disables mirroring
     * Also, mirroring requires at least one active secondary.
     */
    if (ratio == 0 || isMaster->getHosts().size() < 2) {
        return {};
    }

    /**
     * The goal is to mirror every request to approximately `ratio x secondariesCount`.
     */
    const auto secondariesCount = isMaster->getHosts().size() - 1;
    const auto secondariesRatio = ratio * secondariesCount;

    // Mirroring factor is the number of secondaries that will receive the command.
    auto mirroringFactor = std::ceil(secondariesRatio);
    invariant(mirroringFactor > 0 && mirroringFactor <= secondariesCount);

    size_t randVar = rnd();
    const auto normalizedRatio = static_cast<size_t>(secondariesRatio * rndMax / mirroringFactor);

    if (randVar > normalizedRatio) {
        return {};
    }

    auto getEligibleSecondaries = [](auto isMaster) noexcept->std::vector<HostAndPort> {
        auto self = isMaster->getPrimary();
        auto hosts = isMaster->getHosts();
        invariant(hosts.size() > 1);

        std::vector<HostAndPort> potentialTargets;
        for (auto host : hosts) {
            if (host != self) {
                potentialTargets.push_back(host);
            }
        }

        return potentialTargets;
    };

    auto eligibleSecondaries = getEligibleSecondaries(isMaster);
    invariant(!eligibleSecondaries.empty());

    std::mt19937 twisterEngine(randVar);
    std::shuffle(eligibleSecondaries.begin(), eligibleSecondaries.end(), twisterEngine);

    auto it = eligibleSecondaries.begin();
    return std::vector<HostAndPort>(it, it + mirroringFactor);
}

}  // namespace mongo
