// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/query/query_stats/transform_algorithm_gen.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <vector>

namespace mongo {
/**
 * Validate properties of the algorithm field of $queryStats.transformIdentifiers
 */
Status validateAlgo(TransformAlgorithmEnum algorithm);

/**
 * Validate properties of the hmac key field of $queryStats.transformIdentifiers
 */
Status validateHmac(std::vector<uint8_t> hmacKey);
}  // namespace mongo
