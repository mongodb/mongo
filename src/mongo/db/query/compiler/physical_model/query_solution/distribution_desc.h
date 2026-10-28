// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <variant>
#include <vector>

namespace mongo {

/**
 * The output distribution of a node. This is a property of the node itself and describes the shape
 * of the output, independent of whatever mechanism produced it.
 */
struct DistributionDesc {
    enum class Type {
        // A single stream on a single worker.
        Unpartitioned,
        // Rows divided across workers by hash of 'keys'.
        HashPartitioned,
        // Rows divided across workers by range boundaries on 'keys'.
        RangePartitioned,
        // Rows divided cyclically.
        RoundRobin,
        // Every worker holds a full copy of every row.
        Replicated,
    };

    Type type = Type::Unpartitioned;

    // Non-empty if and only if type is HashPartitioned or RangePartitioned.
    std::vector<FieldPath> keys;

    // Construct through these rather than aggregate-initializing, so that no (type, keys) pair
    // outside the invariant above is produced.
    static DistributionDesc unpartitioned() {
        return DistributionDesc{Type::Unpartitioned, {}};
    }

    static DistributionDesc replicated() {
        return DistributionDesc{Type::Replicated, {}};
    }

    static DistributionDesc roundRobin() {
        return DistributionDesc{Type::RoundRobin, {}};
    }

    static DistributionDesc hashOn(std::vector<FieldPath> keys) {
        tassert(13623104, "hash partitioning requires at least one key", !keys.empty());
        return DistributionDesc{Type::HashPartitioned, std::move(keys)};
    }

    static DistributionDesc rangeOn(std::vector<FieldPath> keys) {
        tassert(13623105, "range partitioning requires at least one key", !keys.empty());
        return DistributionDesc{Type::RangePartitioned, std::move(keys)};
    }

    bool operator==(const DistributionDesc& other) const = default;
};

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
        enum class Type {
            // hash(keys) % dop -> destination worker.
            Hash,
            // Range boundaries on 'keys' -> destination worker.
            Range,
            // Cyclic assignment.
            RoundRobin,
        };

        Type type = Type::RoundRobin;

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
        return ExchangeRouting{Partition{Partition::Type::Hash, std::move(keys)}};
    }

    static ExchangeRouting roundRobin() {
        return ExchangeRouting{Partition{Partition::Type::RoundRobin, {}}};
    }
};

/**
 * Maps an exchange's routing mechanism to the distribution its output has.
 */
DistributionDesc toOutputDistribution(const ExchangeRouting& routing);

std::string toString(const DistributionDesc& d);
std::string toString(const ExchangeRouting& routing);

}  // namespace mongo
