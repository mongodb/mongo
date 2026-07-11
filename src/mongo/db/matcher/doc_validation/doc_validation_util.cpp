// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/doc_validation/doc_validation_util.h"

#include "mongo/bson/bson_depth.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::doc_validation_error {
std::unique_ptr<MatchExpression::ErrorAnnotation> createAnnotation(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::string& tag,
    BSONObj annotation,
    const BSONObj& jsonSchemaElement) {
    if (expCtx->getIsParsingCollectionValidator()) {
        return std::make_unique<MatchExpression::ErrorAnnotation>(
            tag, std::move(annotation), jsonSchemaElement);
    } else {
        return nullptr;
    }
}
std::unique_ptr<MatchExpression::ErrorAnnotation> createAnnotation(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    MatchExpression::ErrorAnnotation::Mode mode) {
    if (expCtx->getIsParsingCollectionValidator()) {
        return std::make_unique<MatchExpression::ErrorAnnotation>(mode);
    } else {
        return nullptr;
    }
}

void annotateTreeToIgnoreForErrorDetails(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         MatchExpression* expr) {
    expr->setErrorAnnotation(
        createAnnotation(expCtx, MatchExpression::ErrorAnnotation::Mode::kIgnore));
    for (const auto childExpr : *expr) {
        annotateTreeToIgnoreForErrorDetails(expCtx, childExpr);
    }
}

unsigned int computeMaxAllowedValidationErrorDepth() {
    // The default number of levels a generated error must be below the configured depth to be
    // considered valid.
    static constexpr auto kDefaultOffset = 10u;
    // Only use 'kDefaultOffset' if the configured depth is greater than it. The validation error
    // response will always allow at least 'BSONDepth::kBSONDepthParameterFloor' levels of nesting.
    return BSONDepth::getMaxAllowableDepth() > kDefaultOffset
        ? std::max(BSONDepth::getMaxAllowableDepth() - kDefaultOffset,
                   static_cast<uint32_t>(BSONDepth::kBSONDepthParameterFloor))
        : BSONDepth::kBSONDepthParameterFloor;
}
}  // namespace mongo::doc_validation_error
