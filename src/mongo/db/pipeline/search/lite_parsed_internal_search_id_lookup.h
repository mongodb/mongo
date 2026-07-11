// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup_gen.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * StageParams for DocumentSourceInternalSearchIdLookUp. This class encapsulates the parameters
 * needed to construct a DocumentSourceInternalSearchIdLookUp stage.
 */
class InternalSearchIdLookupStageParams : public StageParams {
public:
    InternalSearchIdLookupStageParams() = default;
    InternalSearchIdLookupStageParams(DocumentSourceIdLookupSpec spec)
        : ownedSpec(std::move(spec)) {}

    static const Id& id;
    Id getId() const final {
        return id;
    }

    const DocumentSourceIdLookupSpec ownedSpec;
};

class LiteParsedInternalSearchIdLookUp final
    : public LiteParsedDocumentSourceDefault<LiteParsedInternalSearchIdLookUp> {
public:
    static constexpr std::string_view kStageName = "$_internalSearchIdLookup"sv;

    static std::unique_ptr<LiteParsedInternalSearchIdLookUp> parse(const NamespaceString& nss,
                                                                   const BSONElement& spec,
                                                                   const LiteParserOptions& opts) {
        uassert(ErrorCodes::FailedToParse,
                "$_internalSearchIdLookup specification must be an object",
                spec.type() == BSONType::object);

        BSONObj specObj = spec.Obj().getOwned();

        // Parse using IDL.
        auto idlSpec = DocumentSourceIdLookupSpec::parse(specObj, IDLParserContext(kStageName));

        return std::make_unique<LiteParsedInternalSearchIdLookUp>(std::move(idlSpec));
    }

    bool isInitialSource() const override {
        return false;
    }

    // All search stages are unsupported on timeseries collections.
    Constraints constraints() const override {
        return {.canRunOnTimeseries = false};
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<InternalSearchIdLookupStageParams>(_ownedSpec);
    }

    const DocumentSourceIdLookupSpec& getSpec() const {
        return _ownedSpec;
    }

    FirstStageViewApplicationPolicy getFirstStageViewApplicationPolicy() const override {
        return FirstStageViewApplicationPolicy::kDoNothing;
    }

    // Fetches the full documents for mongot's _ids without reshaping them.
    bool isSelectionStage() const override {
        return true;
    }

    void bindResolvedNamespace(const ResolvedNamespace& view,
                               const ResolvedNamespaceMap&) override {
        if (view.getNamespace().isEmpty()) {
            // An empty view namespace means this stage is being notified that its pipeline is
            // *not* running on a view. Nothing to bind.
            return;
        }
        _ownedSpec.setViewPipeline(view.getSerializedViewPipeline());
    }

    LiteParsedInternalSearchIdLookUp(DocumentSourceIdLookupSpec spec)
        : LiteParsedDocumentSourceDefault(BSON(kStageName << spec.toBSON()).getOwned()),
          _ownedSpec(std::move(spec)) {}

private:
    DocumentSourceIdLookupSpec _ownedSpec;
};

}  // namespace mongo
