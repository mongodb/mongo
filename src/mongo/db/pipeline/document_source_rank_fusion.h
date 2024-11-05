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
#include "mongo/db/pipeline/document_source_rank_fusion_gen.h"
#include "mongo/db/pipeline/document_source_rank_fusion_inputs_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"

namespace mongo {

/**
 * The $rankFusion stage is syntactic sugar for generating an output of ranked results by combining
 * the results of any number of ranked subpipelines with reciprocal rank fusion.
 *
 * You can see an sample desugared pipeline in ranked_fusion_verbose_replace_root_test.js, but
 * conceptually: $rankFusion, given input pipelines p1 to pX, desugars into the pipeline:
 * - Score-generating stage from p1 (e.g., $vectorSearch).
 * - Via $group and $unwind, for each document returned:
 *     - Add a rank field: first_rank.
 *     - Add a score field: first_score.
 *         - Score is calculated via: 1 / (rank + (rankConstant = 60)).
 * - For each additional pipeline pX add a:
 *     - $unionWith {
 *          The same steps that were run on p1, but for pipeline px:
 *          thus, adding fields xth_score and xth_rank. }
 *          TODO SERVER-95164: change note about naming if applicable.
 * - $group the docs via ID.
 * - Zero null scores.
 * - Prepare the output:
 *      - Add a new “score” field and nest first_score and second_score within it.
 *      - Sort in descending score order.
 */
class DocumentSourceRankFusion final {
public:
    static constexpr StringData kStageName = "$rankFusion"_sd;

    /**
     * Returns a list of stages to execute hybrid scoring with rank fusion.
     */
    static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    class LiteParsed final : public LiteParsedDocumentSourceNestedPipelines {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec);

        LiteParsed(std::string parseTimeName,
                   const NamespaceString& nss,
                   std::vector<LiteParsedPipeline> pipelines)
            : LiteParsedDocumentSourceNestedPipelines(
                  std::move(parseTimeName), nss, std::move(pipelines)) {}


        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return requiredPrivilegesBasic(isMongos, bypassDocumentValidation);
        };
    };

    static StageConstraints constraints() {
        StageConstraints constraints{DocumentSource::StreamType::kStreaming,
                                     DocumentSource::PositionRequirement::kFirst,
                                     DocumentSource::HostTypeRequirement::kLocalOnly,
                                     DocumentSource::DiskUseRequirement::kNoDiskUse,
                                     DocumentSource::FacetRequirement::kNotAllowed,
                                     DocumentSource::TransactionRequirement::kAllowed,
                                     DocumentSource::LookupRequirement::kAllowed,
                                     DocumentSource::UnionRequirement::kAllowed};
        // Tried to get rid of the 'has to be the first stage in the pipeline' error.
        constraints.requiresInputDocSource = false;
        return constraints;
    }

private:
    // It is illegal to construct a DocumentSourceRankFusion directly, use createFromBson() instead.
    DocumentSourceRankFusion() = default;
};

}  // namespace mongo
