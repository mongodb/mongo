// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/util/modules.h"


namespace mongo {

/**
 * This class is used by the aggregation framework and streams enterprise module to perform the
 * document processing needed for stages that do 1:1 document transformation.
 */
class [[MONGO_MOD_PUBLIC]] SingleDocumentTransformationProcessor {
public:
    SingleDocumentTransformationProcessor(std::unique_ptr<TransformerInterface> parsedTransform);

    // Processes the given document and returns the transformed document. The optional 'ctx'
    // parameter carries evaluation state (see EvaluationContext) and defaults to an empty context;
    // when it holds a memory tracker, memory usage observed while evaluating any expressions
    // involved in the transformation is accumulated against it.
    Document process(const Document& input, const EvaluationContext& ctx = {}) const;

    const auto& getTransformer() const {
        return *_parsedTransform;
    }
    auto& getTransformer() {
        return *_parsedTransform;
    }

private:
    // Stores transformation logic.
    std::unique_ptr<TransformerInterface> _parsedTransform;
};

}  // namespace mongo
