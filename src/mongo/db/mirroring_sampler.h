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

#pragma once

#include <functional>
#include <memory>
#include <random>
#include <vector>

#include "mongo/db/repl/is_master_response.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

/**
 * Populates a random subset of eligible secondaries (using `IsMasterResponse`
 * and a ratio) for mirroring. An empty subset for eligible secondaries indicates
 * no mirroring is necessary/possible.
 */
class MirroringSampler final {
public:
    using RandomFunc = std::function<int()>;

    /**
     * Sampler function for mirroring commands to eligible secondaries.
     * The caller must be the primary node of the replica-set.
     * @arg isMaster  replica-set topology that determines mirroring targets.
     * @arg ratio  the mirroring ratio (must be between 0 and 1 inclusive).
     * @arg rnd  the random generator function (default is `std::rand()`).
     * @arg rndMax  the maximum value that `rnd()` returns (default is `RAND_MAX`).
     */
    static std::vector<HostAndPort> getMirroringTargets(
        std::shared_ptr<const repl::IsMasterResponse> isMaster,
        const double ratio,
        RandomFunc rnd = std::rand,
        const int rndMax = RAND_MAX) noexcept;
};

}  // namespace mongo
