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

#pragma once

#include <list>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_score_fusion_gen.h"
#include "mongo/db/pipeline/document_source_score_fusion_inputs_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"

namespace mongo {

/**
 * The $scoreFusion stage is syntactic sugar for generating an output of scored results by combining
 * the results of any number of scored subpipelines with relative score fusion.
 *
 * Given n input pipelines each with a unique name, desugars into a
 * pipeline consisting of:
 * - The first input pipeline (e.g. $vectorSearch).
 * - $replaceRoot and $addFields that for each document returned will:
 *     - Add a score field: <pipeline name>_score (e.g. vs_score).
 *         - Score is calculated as the weight * the score field on the input documents.
 * - n-1 $unionWith stages on the same collection, which take as input pipelines:
 *     - The nth input pipeline.
 *     - $replaceRoot and $addFields which do the same thing as described above.
 * - $group by ID and turn null scores into 0.
 * - $addFields for a 'score' field which will aggregate the n scores for each document.
 * - $sort in descending order.
 */
class DocumentSourceScoreFusion final {
public:
    static constexpr StringData kStageName = "$scoreFusion"_sd;

    /**
     * Returns a list of stages to execute hybrid scoring with score fusion.
     */
    static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    class LiteParsed final : public LiteParsedDocumentSourceNestedPipelines {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options);

        LiteParsed(std::string parseTimeName,
                   const NamespaceString& nss,
                   std::vector<LiteParsedPipeline> pipelines)
            : LiteParsedDocumentSourceNestedPipelines(
                  std::move(parseTimeName), nss, std::move(pipelines)) {}

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return requiredPrivilegesBasic(isMongos, bypassDocumentValidation);
        };

        bool isSearchStage() const final {
            return true;
        }
    };

    // TODO (SERVER-102534): Make ScoreFusionScoringOptions private.

    // The ScoreFusionScoringOptions class validates and stores the normalization,
    // combination.method, and combination.expression fields. combination.expression is not
    // immediately parsed into an expression because any pipelines variables it references will be
    // considered undefined and will therefore throw an error at parsing time.
    // combination.expression will only be parsed into an expression when the enclosing $let var
    // (which defines the pipeline variables) is constructed.
    class ScoreFusionScoringOptions {
    public:
        ScoreFusionScoringOptions(const ScoreFusionSpec& spec) {
            _normalizationMethod = spec.getInput().getNormalization();
            auto& combination = spec.getCombination();
            // The default combination method is avg if no combination method is specified.
            ScoreFusionCombinationMethodEnum combinationMethod =
                ScoreFusionCombinationMethodEnum::kAvg;
            boost::optional<IDLAnyType> combinationExpression = boost::none;
            if (combination.has_value() && combination->getMethod().has_value()) {
                combinationMethod = combination->getMethod().get();
                uassert(10017300,
                        "combination.expression should only be specified when combination.method "
                        "has the value \"expression\"",
                        (combinationMethod != ScoreFusionCombinationMethodEnum::kExpression &&
                         !combination->getExpression().has_value()) ||
                            (combinationMethod == ScoreFusionCombinationMethodEnum::kExpression &&
                             combination->getExpression().has_value()));
                combinationExpression = combination->getExpression();
                uassert(
                    10017301,
                    "both combination.expression and combination.weights cannot be specified",
                    !(combination->getWeights().has_value() && combinationExpression.has_value()));
            }
            _combinationMethod = std::move(combinationMethod);
            _combinationExpression = std::move(combinationExpression);
        }

        ScoreFusionNormalizationEnum getNormalizationMethod() const {
            return _normalizationMethod;
        }

        std::string getNormalizationString(ScoreFusionNormalizationEnum normalization) const {
            switch (normalization) {
                case ScoreFusionNormalizationEnum::kSigmoid:
                    return "sigmoid";
                case ScoreFusionNormalizationEnum::kMinMaxScaler:
                    return "minMaxScaler";
                case ScoreFusionNormalizationEnum::kNone:
                    return "none";
                default:
                    // Only one of the above options can be specified for normalization.
                    MONGO_UNREACHABLE_TASSERT(9467100);
            }
        }

        ScoreFusionCombinationMethodEnum getCombinationMethod() const {
            return _combinationMethod;
        }

        std::string getCombinationMethodString(ScoreFusionCombinationMethodEnum comboMethod) const {
            switch (comboMethod) {
                case ScoreFusionCombinationMethodEnum::kExpression:
                    return "custom expression";
                case ScoreFusionCombinationMethodEnum::kAvg:
                    return "average";
                default:
                    // Only one of the above options can be specified for combination.method.
                    MONGO_UNREACHABLE_TASSERT(9467101);
            }
        }

        boost::optional<IDLAnyType> getCombinationExpression() const {
            return _combinationExpression;
        }

    private:
        // The default normalization value is ScoreFusionCombinationMethodEnum::kNone. The IDL
        // handles the default behavior.
        ScoreFusionNormalizationEnum _normalizationMethod;
        // The default combination.method value is ScoreFusionCombinationMethodEnum::kAvg. The IDL
        // handles the default behavior.
        ScoreFusionCombinationMethodEnum _combinationMethod;
        // This field should only be populated when combination.method has the value
        // ScoreFusionCombinationMethodEnum::kExpression.
        boost::optional<IDLAnyType> _combinationExpression = boost::none;
    };

private:
    // It is illegal to construct a DocumentSourceScoreFusion directly, use createFromBson()
    // instead.
    DocumentSourceScoreFusion() = delete;
};

}  // namespace mongo
