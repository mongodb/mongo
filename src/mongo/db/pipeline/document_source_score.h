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

#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_score_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"

namespace mongo {

/**
 * A projection-like stage, $score will output documents which are the same as the input
 * documents, now with extra metadata.
 *
 * This stage's goal is twofold:
 * - Help satisfy the constraint of $scoreFusion to participate in hybrid search. A valid input
 *   to $scoreFusion (known as a scored selection pipeline) is forbidden from modifying the
 *   actual documents, so this stage aims to allow computing a new thing as metadata without
 *   being considered a modification.
 * - Provide a way to normalize input scores to the same domain (usually between 0 and 1).
 */
class DocumentSourceScore final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$score"_sd;

    /**
     * Create a new $score stage.
     */
    static boost::intrusive_ptr<DocumentSourceScore> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        ScoreSpec spec,
        boost::intrusive_ptr<Expression> parsedScore);

    /**
     * Allows computation of score metadata for non-search pipelines, and also allows weighting or
     * normalizing scores.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Specify stage constraints. $score does not modify any documents.
     */
    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed};
        constraints.noFieldModifications = true;
        return constraints;
    }

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    DocumentSourceType getType() const override {
        return DocumentSourceType::kScore;
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    /**
     * This stage can be run in parallel.
     */
    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    ScoreSpec getSpec() const {
        return _spec;
    }

    boost::intrusive_ptr<Expression> getScore() const {
        return _parsedScore;
    }

private:
    // It is illegal to construct a DocumentSourceScore directly, use createFromBson
    // instead. Added a constructor only for use in DocumentSourceScore implementation.
    DocumentSourceScore(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                        ScoreSpec spec,
                        boost::intrusive_ptr<Expression> parsedScore);
    GetNextResult doGetNext() final;

    ScoreSpec _spec;
    boost::intrusive_ptr<Expression> _parsedScore;
};

}  // namespace mongo
