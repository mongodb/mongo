// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_document_source_nested_pipelines.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_NEEDS_REPLACEMENT]] mongo {
using namespace std::literals::string_view_literals;

/**
 * Parameters produced by LiteParsedGraphLookUp::getStageParams() and consumed by
 * DocumentSourceGraphLookUp::createFromStageParams(). Carries the lite-parsed values so the
 * full parse step only needs to handle what requires an ExpressionContext (the startWith
 * Expression and MatchExpression validation for restrictSearchWithMatch).
 */
class GraphLookUpStageParams : public DefaultStageParams {
public:
    GraphLookUpStageParams(NamespaceString from,
                           boost::optional<FieldPath> as,
                           boost::optional<FieldPath> connectFromField,
                           boost::optional<FieldPath> connectToField,
                           boost::optional<BSONElement> startWith,
                           boost::optional<BSONObj> additionalFilter,
                           boost::optional<FieldPath> depthField,
                           boost::optional<long long> maxDepth,
                           BSONElement originalBson,
                           boost::optional<OwnedLiteParsedPipeline> liteParsedPipeline = {})
        : DefaultStageParams(originalBson),
          from(std::move(from)),
          as(std::move(as)),
          connectFromField(std::move(connectFromField)),
          connectToField(std::move(connectToField)),
          startWith(startWith),
          additionalFilter(std::move(additionalFilter)),
          depthField(std::move(depthField)),
          maxDepth(maxDepth),
          liteParsedPipeline(std::move(liteParsedPipeline)) {}

    static const Id& id;
    Id getId() const final {
        return id;
    }

    NamespaceString from;
    // 'as', 'connectFromField', 'connectToField', and 'startWith' are required by $graphLookup,
    // but the lite-parse stage only needs 'from' to compute privileges. Required-field
    // presence is enforced by DocumentSourceGraphLookUp::createFromStageParams.
    boost::optional<FieldPath> as;
    boost::optional<FieldPath> connectFromField;
    boost::optional<FieldPath> connectToField;
    boost::optional<BSONElement> startWith;
    boost::optional<BSONObj> additionalFilter;
    boost::optional<FieldPath> depthField;
    boost::optional<long long> maxDepth;
    // TODO SERVER-127906 Remove the LPP from StageParams once the LP->DS->exec pipeline translation
    // bridges the subpipeline across phase boundaries properly.
    // Set at lite-parse time; carries the view pipeline (nullopt for regular collections)
    // to DocumentSourceGraphLookUp::createFromStageParams.
    boost::optional<OwnedLiteParsedPipeline> liteParsedPipeline;
};

/**
 * Lite-parse representation of $graphLookup
 */
class LiteParsedGraphLookUp final
    : public LiteParsedDocumentSourceNestedPipelines<LiteParsedGraphLookUp> {
public:
    static constexpr std::string_view kStageName = "$graphLookup"sv;

    static std::unique_ptr<LiteParsedGraphLookUp> parse(const NamespaceString& nss,
                                                        const BSONElement& spec,
                                                        const LiteParserOptions& options);

    LiteParsedGraphLookUp(const BSONElement& spec,
                          NamespaceString foreignNss,
                          boost::optional<FieldPath> as,
                          boost::optional<FieldPath> connectFromField,
                          boost::optional<FieldPath> connectToField,
                          boost::optional<BSONObj> startWith,
                          boost::optional<BSONObj> additionalFilter,
                          boost::optional<FieldPath> depthField,
                          boost::optional<long long> maxDepth,
                          const LiteParserOptions& options,
                          boost::optional<OwnedLiteParsedPipeline> fromPipeline = {});

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final;

    Status checkShardedForeignCollAllowed(const NamespaceString& nss,
                                          bool inMultiDocumentTransaction) const override;

    std::unique_ptr<StageParams> getStageParams() const override;

protected:
    bool needsViewSubpipelineMaterialized() const override {
        return _pipelines.empty();
    }

private:
    boost::optional<FieldPath> _as;
    boost::optional<FieldPath> _connectFromField;
    boost::optional<FieldPath> _connectToField;
    boost::optional<BSONObj> _startWith;
    boost::optional<BSONObj> _additionalFilter;
    boost::optional<FieldPath> _depthField;
    boost::optional<long long> _maxDepth;
};

}  // namespace mongo
