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
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceScore::DocumentSourceScore(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                         ScoreSpec spec,
                                         boost::intrusive_ptr<Expression> parsedScore,
                                         boost::intrusive_ptr<Expression> parsedNormalizeFunction,
                                         double parsedWeight)
    : DocumentSource(kStageName, pExpCtx),
      _spec(spec),
      _parsedScore(parsedScore),
      _parsedNormalizeFunction(parsedNormalizeFunction),
      _parsedWeight(parsedWeight) {}

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

constexpr StringData DocumentSourceScore::kStageName;

DocumentSource::GetNextResult DocumentSourceScore::doGetNext() {
    // Get the next input document.
    auto input = pSource->getNext();

    // If input is not advanced, return the input.
    if (!input.isAdvanced()) {
        return input;
    }

    Document currentDoc = input.getDocument();
    // Evaluate and validate the scored expression.
    Value scoreValue = _parsedScore->evaluate(currentDoc, &(pExpCtx->variables));
    uassert(9484101,
            "Invalid expression or evaluated expression is not a valid double",
            isNumericBSONType(scoreValue.getType()));
    double scoreDouble = scoreValue.getDouble();

    // Validate and execute the specified normalize function.
    if (_parsedNormalizeFunction) {
        Value evaluatedSigmoid =
            _parsedNormalizeFunction->evaluate(currentDoc, &(pExpCtx->variables));
        scoreDouble = evaluatedSigmoid.getDouble();
    }

    // TODO SERVER-94600: Handle minMaxScaler expression behavior

    // If we don't enter above cases then normalize function is "none." Should have validated
    // before this point that _parsedNormalizeFunction is one of the normalize function types.

    // Calculate score with the specified weight (bounds: [0, 1]). Default is 1.0.
    scoreDouble = scoreDouble * _parsedWeight;

    // Store it in score's metadata (document must be mutable in order for score to be set).
    MutableDocument output(std::move(currentDoc));
    output.metadata().setScore(scoreDouble);

    return output.freeze();
}

Value DocumentSourceScore::serialize(const SerializationOptions& opts) const {
    return Value(Document{{kStageName, _spec.toBSON()}});
}

void DocumentSourceScore::addVariableRefs(std::set<Variables::Id>* refs) const {
    expression::addVariableRefs(_parsedScore.get(), refs);
}

intrusive_ptr<DocumentSourceScore> DocumentSourceScore::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    ScoreSpec spec,
    boost::intrusive_ptr<Expression> parsedScore,
    boost::intrusive_ptr<Expression> parsedNormalizeFunction,
    double parsedWeight) {
    intrusive_ptr<DocumentSourceScore> source(
        new DocumentSourceScore(pExpCtx, spec, parsedScore, parsedNormalizeFunction, parsedWeight));
    return source;
}

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

    // Parse "score" (required field).
    auto score = [&]() -> intrusive_ptr<Expression> {
        auto score = spec.getScore();
        return Expression::parseOperand(
            pExpCtx.get(), score.getElement(), pExpCtx->variablesParseState);
    }();

    // Parse "weight" (optional field). If not specified, default is 1.0.
    double weight = spec.getWeight().value_or(1.0);

    // Parse normalizeFunction once an instance of DocumentSourceScore created. Assume "none" for
    // now.
    boost::intrusive_ptr<Expression> normalizeFunction = nullptr;
    boost::optional<mongo::NormalizeFunctionEnum> normalizeFunctionField =
        spec.getNormalizeFunction();
    bool normFuncExists = normalizeFunctionField.is_initialized();

    // Need a DocumentSourceScore object to access fields within static method (cannot directly
    // access member functions otw). Intrusive pointer manages object destruction.
    intrusive_ptr<DocumentSourceScore> doc =
        create(pExpCtx, spec, std::move(score), std::move(normalizeFunction), weight);

    // Parse "normalizeFunction" (optional field). If not specified, default is "sigmoid."

    // Parsing logic done after creating an instance of DocumentSourceScore so spec member
    // variable can be accessed in parseExpressionSigmoid().
    if ((!normFuncExists) ||
        (normFuncExists && *normalizeFunctionField == NormalizeFunctionEnum::kSigmoid)) {
        normalizeFunction = ExpressionSigmoid::parseExpressionSigmoid(
            pExpCtx.get(), doc->getSpec().getScore().getElement(), pExpCtx->variablesParseState);
        doc->setNormalizeFunction(normalizeFunction);
    }
    return doc;
}

}  // namespace mongo
