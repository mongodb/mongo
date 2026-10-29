// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_score_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <set>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(Score);
class ScoreLiteParsed final : public LiteParsedDocumentSourceDefault<ScoreLiteParsed> {
public:
    ScoreLiteParsed(const BSONElement& originalBson)
        : LiteParsedDocumentSourceDefault<ScoreLiteParsed>(originalBson) {}

    static std::unique_ptr<ScoreLiteParsed> parse(const NamespaceString& nss,
                                                  const BSONElement& spec,
                                                  const LiteParserOptions& options) {
        return std::make_unique<ScoreLiteParsed>(spec);
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<ScoreStageParams>(_originalBson);
    }

    // $score computes score metadata for each document.
    bool isScoredStage() const final {
        return true;
    }

    // $score only sets metadata, it does not modify document fields.
    bool isSelectionStage() const final {
        return true;
    }

    bool isScoreDetailsStage() const final {
        return _originalBson.isABSONObj() && _originalBson.Obj().getBoolField("scoreDetails");
    }
};

/**
 * $score computes the "score" metadata field based on some input Expression, without making any
 * modifications to the non-metadata fields of the original document.
 *
 * This stage's goal is twofold:
 * - Help satisfy the constraint of $scoreFusion to participate in hybrid search. A valid input
 *   to $scoreFusion (known as a scored selection pipeline) is forbidden from modifying the
 *   actual documents, so this stage aims to allow computing a new thing as metadata without
 *   being considered a modification.
 * - Provide a way to normalize input scores to the same domain (usually between 0 and 1).
 */
class DocumentSourceScore final {
public:
    static constexpr std::string_view kStageName = "$score"sv;

    static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    // It is illegal to construct a DocumentSourceScore directly, use createFromBson() instead.
    DocumentSourceScore() = default;
};

}  // namespace mongo
