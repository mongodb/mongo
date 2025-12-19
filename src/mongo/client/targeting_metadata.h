/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <cstdint>
#include <vector>

namespace MONGO_MOD_PUBLIC mongo {

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
}  // namespace MONGO_MOD_PUBLIC mongo
