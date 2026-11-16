// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/util/named_enum.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <variant>
#include <vector>

namespace mongo {

/**
 * How a node's output is spread across workers.
 *
 *   - Unpartitioned: a single stream on a single worker.
 *   - HashPartitioned: rows divided across workers by hash of 'keys'.
 *   - RangePartitioned: rows divided across workers by range boundaries on 'keys'.
 *   - RoundRobin: rows divided cyclically.
 *   - Replicated: every worker holds a full copy of every row.
 */
#define DISTRIBUTION_TYPE_TABLE(X)          \
    X(Unpartitioned, "unpartitioned")       \
    X(HashPartitioned, "hashPartitioned")   \
    X(RangePartitioned, "rangePartitioned") \
    X(RoundRobin, "roundRobin")             \
    X(Replicated, "replicated")

QUERY_UTIL_NAMED_ENUM_DEFINE(DistributionType, DISTRIBUTION_TYPE_TABLE)
#undef DISTRIBUTION_TYPE_TABLE

/**
 * The output distribution of a node. This is a property of the node itself and describes the shape
 * of the output, independent of whatever mechanism produced it.
 */
struct DistributionDesc {
    DistributionType type = DistributionType::Unpartitioned;

    // Non-empty if and only if type is HashPartitioned or RangePartitioned.
    std::vector<FieldPath> keys;

    // Construct through these rather than aggregate-initializing, so that no (type, keys) pair
    // outside the invariant above is produced.
    static DistributionDesc unpartitioned() {
        return DistributionDesc{DistributionType::Unpartitioned, {}};
    }

    static DistributionDesc replicated() {
        return DistributionDesc{DistributionType::Replicated, {}};
    }

    static DistributionDesc roundRobin() {
        return DistributionDesc{DistributionType::RoundRobin, {}};
    }

    static DistributionDesc hashOn(std::vector<FieldPath> keys) {
        tassert(13623104, "hash partitioning requires at least one key", !keys.empty());
        return DistributionDesc{DistributionType::HashPartitioned, std::move(keys)};
    }

    static DistributionDesc rangeOn(std::vector<FieldPath> keys) {
        tassert(13623105, "range partitioning requires at least one key", !keys.empty());
        return DistributionDesc{DistributionType::RangePartitioned, std::move(keys)};
    }

    bool operator==(const DistributionDesc& other) const = default;
};

/**
 * How an exchange divides rows across workers.
 *
 *   - Hash: hash(keys) % dop -> destination worker.
 *   - Range: range boundaries on 'keys' -> destination worker.
 *   - RoundRobin: cyclic assignment.
 */
#define EXCHANGE_PARTITION_TYPE_TABLE(X) \
    X(Hash, "hash")                      \
    X(Range, "range")                    \
    X(RoundRobin, "roundRobin")

QUERY_UTIL_NAMED_ENUM_DEFINE(ExchangePartitionType, EXCHANGE_PARTITION_TYPE_TABLE)
#undef EXCHANGE_PARTITION_TYPE_TABLE

/**
 * How an ExchangeNode moves documents between workers. This describes an action the
 * exchange performs as opposed to the shape of any node's output.
 */
struct ExchangeRouting {
    struct Gather {
        // Only a gather can preserve order: a repartition scatters rows across streams and destroys
        // any global ordering the input had.
        bool sortPreserving = false;
    };

    // A full copy of every row to every worker.
    struct Broadcast {};

    // Rows divided across workers.
    struct Partition {
        ExchangePartitionType type = ExchangePartitionType::RoundRobin;

        std::vector<FieldPath> keys;
    };

    std::variant<Gather, Broadcast, Partition> mode;

    static ExchangeRouting gather(bool sortPreserving) {
        return ExchangeRouting{Gather{sortPreserving}};
    }

    static ExchangeRouting broadcast() {
        return ExchangeRouting{Broadcast{}};
    }

    static ExchangeRouting hashOn(std::vector<FieldPath> keys) {
        return ExchangeRouting{Partition{ExchangePartitionType::Hash, std::move(keys)}};
    }

    static ExchangeRouting roundRobin() {
        return ExchangeRouting{Partition{ExchangePartitionType::RoundRobin, {}}};
    }
};

/**
 * Maps an exchange's routing mechanism to the distribution its output has.
 */
DistributionDesc toOutputDistribution(const ExchangeRouting& routing);

/**
 * The name of a routing mechanism: "gather", "broadcast", or the partition type. This names the
 * mode alone and leaves out a partition's keys, unlike toString() below, which renders the whole
 * routing.
 */
std::string_view routingModeName(const ExchangeRouting& routing);

std::string toString(const DistributionDesc& d);
std::string toString(const ExchangeRouting& routing);

}  // namespace mongo
