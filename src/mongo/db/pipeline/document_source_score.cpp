/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_score.h"
#include "mongo/db/pipeline/document_source_score_gen.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

using boost::intrusive_ptr;

/** Register $score as a DocumentSource without feature flag and check that the hybrid scoring
 * feature flag is enabled in createFromBson() instead of via
 * REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG. This method of feature flag checking avoids hitting
 * QueryFeatureNotAllowed and duplicate parser map errors in $scoreFusion tests ($scoreFusion is
 * gated behind the same feature flag).
 */
REGISTER_DOCUMENT_SOURCE(score,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceScore::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);

namespace {
intrusive_ptr<Expression> buildMetadataExpression(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                                  const ScoreSpec& spec) {

    intrusive_ptr<Expression> scoreAndNormalizeExpr = [&]() {
        switch (spec.getNormalizeFunction()) {
            // $sigomid will recursively parse and nest the score Expression.
            case NormalizeFunctionEnum::kSigmoid:
                return ExpressionSigmoid::parseExpressionSigmoid(
                    pExpCtx.get(), spec.getScore().getElement(), pExpCtx->variablesParseState);
            // TODO SERVER-94600: Handle minMaxScaler expression behavior.
            // The default case is no normalization, so parse just the score operator itself.
            default:
                return Expression::parseOperand(
                    pExpCtx.get(), spec.getScore().getElement(), pExpCtx->variablesParseState);
        }
    }();

    if (spec.getWeight() == 1) {
        return scoreAndNormalizeExpr;
    }

    std::vector<intrusive_ptr<Expression>> children = {
        std::move(scoreAndNormalizeExpr),
        make_intrusive<ExpressionConstant>(pExpCtx.get(), Value(spec.getWeight()))};
    return make_intrusive<ExpressionMultiply>(pExpCtx.get(), std::move(children));
}
}  // namespace


intrusive_ptr<DocumentSource> DocumentSourceScore::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::QueryFeatureNotAllowed,
            "$score is not allowed in the current configuration. You may need to enable the "
            "correponding feature flag",
            feature_flags::gFeatureFlagSearchHybridScoring.isEnabledUseLatestFCVWhenUninitialized(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);
    auto spec = ScoreSpec::parse(IDLParserContext(kStageName), elem.embeddedObject());

    boost::intrusive_ptr<Expression> expr = buildMetadataExpression(pExpCtx, spec);

    return DocumentSourceSetMetadata::create(
        pExpCtx, std::move(expr), DocumentMetadataFields::MetaType::kScore);
}

}  // namespace mongo
