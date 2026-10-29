// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/analyze_shard_key_cmd_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace analyze_shard_key {

/**
 * Returns metrics about the characteristics of the shard key (i.e. the cardinality, frequency
 * and monotonicity) if the shard key has a supporting index.
 */
boost::optional<KeyCharacteristicsMetrics> calculateKeyCharacteristicsMetrics(
    OperationContext* opCtx,
    const UUID& analyzeShardKeyId,
    const NamespaceString& nss,
    const UUID& collUuid,
    const KeyPattern& shardKey,
    boost::optional<double> sampleRate,
    boost::optional<int64_t> sampleSize);

/**
 * Returns metrics about the read and write distribution based on sampled queries.
 */
std::pair<ReadDistributionMetrics, WriteDistributionMetrics> calculateReadWriteDistributionMetrics(
    OperationContext* opCtx,
    const UUID& analyzeShardKeyId,
    const NamespaceString& nss,
    const UUID& collUuid,
    const KeyPattern& shardKey);

}  // namespace analyze_shard_key
}  // namespace mongo
