// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/physical_model/query_solution/distribution_desc.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace mongo::parallelism_planner {

/**
 * The distribution required by a parent for a child edge in a QuerySolution tree.
 */
struct RequiredDistribution {
    enum class Type {
        // Impose nothing.
        Any,
        // A single stream on a single worker.
        Unpartitioned,
        // Hash-partitioned on 'keys'.
        HashPartitioned,
        // Documents are replicated across all workers.
        Replicated,
    };

    Type type = Type::Any;

    // Set if and only if type == HashPartitioned.
    std::vector<FieldPath> keys;

    static RequiredDistribution any() {
        return {Type::Any, {}};
    }
    static RequiredDistribution unpartitioned() {
        return {Type::Unpartitioned, {}};
    }
    static RequiredDistribution replicated() {
        return {Type::Replicated, {}};
    }
    static RequiredDistribution hashOn(std::vector<FieldPath> keys) {
        return {Type::HashPartitioned, std::move(keys)};
    }
};

std::string toString(const RequiredDistribution& r);

/**
 * Returns whether the 'produced' distribution satisfies the 'required' distribution.
 */
bool satisfies(const DistributionDesc& produced, const RequiredDistribution& required);

}  // namespace mongo::parallelism_planner
