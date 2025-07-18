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

#include "mongo/db/query/compiler/ce/exact/exact_cardinality_impl.h"

#include "mongo/db/query/stage_builder/stage_builder_util.h"


namespace mongo::ce {
CEResult ExactCardinalityImpl::populateCardinalities(
    const QuerySolutionNode* node,
    const PlanStage* execStage,
    cost_based_ranker::EstimateMap& cardinalities) const {
    const auto commonStats = execStage->getCommonStats();
    cost_based_ranker::QSNEstimate card{
        .outCE = CardinalityEstimate{CardinalityType{(double)commonStats->advanced},
                                     EstimationSource::Sampling}};
    // If we are at a leaf node, we must record inCE as well. We get this from the SpecificStats.
    if (execStage->getChildren().empty()) {
        switch (execStage->stageType()) {
            case STAGE_COLLSCAN: {
                auto stats = static_cast<const CollectionScanStats*>(execStage->getSpecificStats());
                card.inCE = CardinalityEstimate{CardinalityType{(double)stats->docsTested},
                                                EstimationSource::Sampling};
                break;
            }
            case STAGE_IXSCAN: {
                auto stats = static_cast<const IndexScanStats*>(execStage->getSpecificStats());
                card.inCE = CardinalityEstimate{CardinalityType{(double)stats->keysExamined},
                                                EstimationSource::Sampling};
                break;
            }
            default:
                // Any other leaf nodes should not get to CBR as they are not supported.
                MONGO_UNREACHABLE_TASSERT(10659800);
        }
    }
    CardinalityEstimate res{card.outCE};
    cardinalities.emplace(node, std::move(card));

    tassert(10659801,
            "A QSN should have the same number of children as its corresponding execution stage",
            node->children.size() == execStage->getChildren().size());
    for (size_t i = 0; i < node->children.size(); ++i) {
        const auto childRes = populateCardinalities(
            node->children[i].get(), execStage->getChildren()[i].get(), cardinalities);
        if (!childRes.isOK()) {
            return childRes;
        }
    }
    return res;
}
CEResult ExactCardinalityImpl::calculateExactCardinality(
    const QuerySolution& plan, cost_based_ranker::EstimateMap& cardinalities) const {
    const auto node = plan.root();
    QuerySolution qs;
    qs.setRoot(node->clone());
    WorkingSet ws;
    WorkingSetID id = ws.allocate();
    auto root = stage_builder::buildClassicExecutableTree(
        _opCtx, VariantCollectionPtrOrAcquisition(_coll), _cq, qs, &ws);
    while (!root->isEOF()) {
        root->work(&id);
    }

    return populateCardinalities(node, root.get(), cardinalities);
}
}  // namespace mongo::ce
