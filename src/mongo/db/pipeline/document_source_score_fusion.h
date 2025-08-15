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

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_score_fusion_gen.h"
#include "mongo/db/pipeline/document_source_score_fusion_inputs_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"

#include <list>

#include <boost/smart_ptr/intrusive_ptr.hpp>

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

    // Name of single top-level field object used to track all internal fields we need
    // intermediate to the desugar.
    // One field object that holds all internal intermediate variables during desugar,
    // like each input pipeline's individual score or scoreDetails.
    static constexpr StringData kScoreFusionInternalFieldsName =
        "_internal_scoreFusion_internal_fields"_sd;

    // One field object to encapsulate the unmodified user's doc from the queried collection.
    static constexpr StringData kScoreFusionDocsFieldName = "_internal_scoreFusion_docs"_sd;

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

        bool requiresAuthzChecks() const override {
            return false;
        }

        bool isSearchStage() const final {
            return _pipelines[0].hasSearchStage();
        }

        bool isHybridSearchStage() const final {
            return true;
        }
    };

private:
    // It is illegal to construct a DocumentSourceScoreFusion directly, use createFromBson()
    // instead.
    DocumentSourceScoreFusion() = delete;
};

}  // namespace mongo
