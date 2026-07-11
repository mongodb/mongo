// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo::doc_validation_error {
/**
 * Set of functions which create an ErrorAnnotation provided that a validator expression is being
 * parsed.
 */
std::unique_ptr<MatchExpression::ErrorAnnotation> createAnnotation(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::string& tag,
    BSONObj annotation,
    const BSONObj& jsonSchemaElement = BSONObj());

std::unique_ptr<MatchExpression::ErrorAnnotation> createAnnotation(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    MatchExpression::ErrorAnnotation::Mode mode);

/**
 * Utility which tags an entire tree with 'AnnotationMode::kIgnore'.
 */
void annotateTreeToIgnoreForErrorDetails(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         MatchExpression* expr);

/**
 * Compute the maximum allowed depth for a validation error. Since the generated error will be
 * included in a command response BSONObj, the maximum depth for validation errors has to be
 * slightly less deep than the maximum allowed depth for BSONObjs.
 */
unsigned int computeMaxAllowedValidationErrorDepth();
}  // namespace mongo::doc_validation_error
