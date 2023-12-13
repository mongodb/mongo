/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/containers.h"
namespace mongo::optimizer::cascades {

/**
 * Map of rewrite type to rewrite priority.
 */
using LogicalRewriteSet = opt::unordered_map<LogicalRewriteType, double>;

const LogicalRewriteSet kDefaultExplorationSet = {
    {LogicalRewriteType::GroupByExplore, 1},
    {LogicalRewriteType::SargableSplit, 2},
    {LogicalRewriteType::FilterRIDIntersectReorder, 2},
    {LogicalRewriteType::EvaluationRIDIntersectReorder, 2}};

const LogicalRewriteSet kDefaultSubstitutionSet = {
    {LogicalRewriteType::FilterEvaluationReorder, 1},
    {LogicalRewriteType::FilterCollationReorder, 1},
    {LogicalRewriteType::EvaluationCollationReorder, 1},
    {LogicalRewriteType::EvaluationLimitSkipReorder, 1},

    {LogicalRewriteType::FilterGroupByReorder, 1},
    {LogicalRewriteType::GroupCollationReorder, 1},

    {LogicalRewriteType::FilterUnwindReorder, 1},
    {LogicalRewriteType::EvaluationUnwindReorder, 1},
    {LogicalRewriteType::UnwindCollationReorder, 1},

    {LogicalRewriteType::FilterExchangeReorder, 1},
    {LogicalRewriteType::ExchangeEvaluationReorder, 1},

    {LogicalRewriteType::FilterUnionReorder, 1},

    {LogicalRewriteType::CollationMerge, 1},
    {LogicalRewriteType::LimitSkipMerge, 1},

    {LogicalRewriteType::SargableFilterReorder, 1},
    {LogicalRewriteType::SargableEvaluationReorder, 1},
    {LogicalRewriteType::SargableDisjunctiveReorder, 1},

    {LogicalRewriteType::FilterValueScanPropagate, 1},
    {LogicalRewriteType::EvaluationValueScanPropagate, 1},
    {LogicalRewriteType::SargableValueScanPropagate, 1},
    {LogicalRewriteType::CollationValueScanPropagate, 1},
    {LogicalRewriteType::LimitSkipValueScanPropagate, 1},
    {LogicalRewriteType::ExchangeValueScanPropagate, 1},

    {LogicalRewriteType::LimitSkipSubstitute, 1},

    {LogicalRewriteType::FilterSubstitute, 2},
    {LogicalRewriteType::EvaluationSubstitute, 2},
    {LogicalRewriteType::SargableMerge, 2}};

// Alternate list of rewrites which avoids splitting Filter nodes into chains of Filter nodes, and
// does not generate SargableNodes. This allows us to skip unnecessary work when we know we cannot
// obtain an index scan plan.
const LogicalRewriteSet kUnindexedSubstitutionSet = {
    {LogicalRewriteType::FilterEvaluationReorder, 1},
    {LogicalRewriteType::FilterCollationReorder, 1},
    {LogicalRewriteType::EvaluationCollationReorder, 1},
    {LogicalRewriteType::EvaluationLimitSkipReorder, 1},

    {LogicalRewriteType::FilterGroupByReorder, 1},
    {LogicalRewriteType::GroupCollationReorder, 1},

    {LogicalRewriteType::FilterUnwindReorder, 1},
    {LogicalRewriteType::EvaluationUnwindReorder, 1},
    {LogicalRewriteType::UnwindCollationReorder, 1},

    {LogicalRewriteType::FilterExchangeReorder, 1},
    {LogicalRewriteType::ExchangeEvaluationReorder, 1},

    {LogicalRewriteType::FilterUnionReorder, 1},

    {LogicalRewriteType::CollationMerge, 1},
    {LogicalRewriteType::LimitSkipMerge, 1},

    {LogicalRewriteType::FilterValueScanPropagate, 1},
    {LogicalRewriteType::EvaluationValueScanPropagate, 1},
    {LogicalRewriteType::CollationValueScanPropagate, 1},
    {LogicalRewriteType::LimitSkipValueScanPropagate, 1},
    {LogicalRewriteType::ExchangeValueScanPropagate, 1},

    {LogicalRewriteType::LimitSkipSubstitute, 1},

    {LogicalRewriteType::FilterSimplify, 2},
};

// Alternate list of Exploration rewrites relevant in unindexed case.
const LogicalRewriteSet kUnindexedExplorationSet = {
    {LogicalRewriteType::GroupByExplore, 1},
};

}  // namespace mongo::optimizer::cascades
