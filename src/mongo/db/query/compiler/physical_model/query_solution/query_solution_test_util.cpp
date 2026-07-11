// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution_test_util.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cardinality_estimator.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_test_utils.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/platform/compiler.h"

namespace mongo {

/**
 * Make a minimal IndexEntry from just a key pattern. A dummy name will be added if none provided.
 */
IndexEntry buildSimpleIndexEntry(const BSONObj& kp, std::string name) {
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            IndexConfig::kLatestIndexVersion,
            false,
            {},
            {},
            false,
            false,
            CoreIndexInfo::Identifier(std::move(name)),
            {},
            nullptr};
}

}  // namespace mongo
