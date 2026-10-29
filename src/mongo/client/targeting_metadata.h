// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <cstdint>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Additional information used to inform remote command targeting / server selection.
 */
struct TargetingMetadata {
    struct Stats {
        /**
         * Counter of how many times targeting did not select a deprioritized server using this
         * metadata. This counter is not increased for targeting performed without any deprioritized
         * servers.
         */
        Atomic<int64_t> numTargetingAvoidedDeprioritized;
    };

    /**
     * List of servers that should not be targeted, if possible.
     * If there are no other suitable, non-deprioritized servers, then a deprioritized server may
     * still be selected.
     */
    std::vector<HostAndPort> deprioritizedServers;

    // If null, stats will not be recorded.
    std::shared_ptr<Stats> stats = {};
};
}  // namespace mongo
