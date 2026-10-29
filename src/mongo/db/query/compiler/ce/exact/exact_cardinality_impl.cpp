// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/exact/exact_cardinality_impl.h"

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/ce_utils.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"


namespace mongo::ce {
CEResult ExactCardinalityImpl::populateCardinalities(
    const QuerySolutionNode* node,
    const PlanStage* execStage,
    cost_based_ranker::EstimateMap& cardinalities) {

    auto stageType = execStage->stageType();

    if (cost_based_ranker::isNodeUnsupportedByCBR(stageType)) {
        // These stages will fallback to multiplanning.
        return Status(ErrorCodes::UnsupportedCbrNode, "encountered unsupported stages");
    } else if (cost_based_ranker::isNodeUnexpectedByCBR(stageType)) {
        // These stages should never reach the cardinality estimator.
        tasserted(12039702,
                  str::stream{} << "Encountered " << stageType
                                << " stage in ExactCE which should be unreachable");
    }

    const auto commonStats = execStage->getCommonStats();
    cost_based_ranker::QSNEstimate card{CardinalityEstimate{
        CardinalityType{(double)commonStats->advanced}, EstimationSource::Code}};
    // If we are at a leaf node, we must record inCE as well. We get this from the SpecificStats.
    if (execStage->getChildren().empty()) {
        // TODO SERVER-99075: Add a case for distinct scan here
        switch (stageType) {
            case STAGE_COLLSCAN: {
                auto stats = static_cast<const CollectionScanStats*>(execStage->getSpecificStats());
                card.inCE = CardinalityEstimate{CardinalityType{(double)stats->docsTested},
                                                EstimationSource::Code};
                break;
            }
            case STAGE_IXSCAN: {
                auto stats = static_cast<const IndexScanStats*>(execStage->getSpecificStats());
                card.inCE = CardinalityEstimate{CardinalityType{(double)stats->keysExamined},
                                                EstimationSource::Code};
                break;
            }
            case STAGE_EOF: {
                // The in CE for the EOF stage is always 0.
                card.inCE = cost_based_ranker::zeroCE;
                break;
            }
            default:
                // Any other leaf nodes should not get to CBR as they are not supported.
                MONGO_UNREACHABLE_TASSERT(10659800);
        }
    }
    CardinalityEstimate res{card.outCE};
    cardinalities.emplace(node, std::make_unique<cost_based_ranker::QSNEstimate>(std::move(card)));

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
        _opCtx, CollectionAcquisition(_coll), _cq, qs, &ws);
    while (!root->isEOF()) {
        root->work(&id);
    }

    return populateCardinalities(node, root.get(), cardinalities);
}
}  // namespace mongo::ce
