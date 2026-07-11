// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/pipeline_test_util.h"

#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"

namespace mongo {

std::unique_ptr<Pipeline> normalizeMatchStageInPipeline(std::unique_ptr<Pipeline> pipeline) {
    for (auto&& source : pipeline->getSources()) {
        if (auto matchStage = dynamic_cast<DocumentSourceMatch*>(source.get())) {
            auto matchProcessor = matchStage->getMatchProcessor();
            auto normalizedExpr =
                normalizeMatchExpression(std::move(matchProcessor->getExpression()));
            matchProcessor->setExpression(std::move(normalizedExpr));
        }
    }

    return pipeline;
}

}  // namespace mongo
