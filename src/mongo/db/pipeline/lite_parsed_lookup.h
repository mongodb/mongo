// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_document_source_nested_pipelines.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_NEEDS_REPLACEMENT]] mongo {
using namespace std::literals::string_view_literals;

class LookUpStageParams : public DefaultStageParams {
public:
    LookUpStageParams(NamespaceString fromNss,
                      std::string as,
                      std::vector<BSONObj> pipeline,
                      BSONObj letVariables,
                      boost::optional<std::string> localField,
                      boost::optional<std::string> foreignField,
                      boost::optional<BSONObj> unwindSpec,
                      bool hasForeignDB,
                      bool isHybridSearch,
                      BSONObj ownedBsonObj,
                      boost::optional<StageParamsPipeline> subpipelineStageParams = boost::none,
                      boost::optional<int64_t> internalFieldMatchPipelineIdx = boost::none,
                      bool internalFromIsAView = false,
                      bool noUserPipeline = false,
                      FirstStageViewApplicationPolicy subpipelineViewPolicy =
                          FirstStageViewApplicationPolicy::kDefaultPrepend)
        : DefaultStageParams(ownedBsonObj.firstElement()),
          fromNss(std::move(fromNss)),
          as(std::move(as)),
          pipeline(std::move(pipeline)),
          letVariables(std::move(letVariables)),
          localField(std::move(localField)),
          foreignField(std::move(foreignField)),
          unwindSpec(std::move(unwindSpec)),
          hasForeignDB(hasForeignDB),
          isHybridSearch(isHybridSearch),
          subpipelineStageParams(std::move(subpipelineStageParams)),
          internalFieldMatchPipelineIdx(std::move(internalFieldMatchPipelineIdx)),
          internalFromIsAView(internalFromIsAView),
          noUserPipeline(noUserPipeline),
          subpipelineViewPolicy(subpipelineViewPolicy),
          _ownedOriginalBson(std::move(ownedBsonObj)) {}

    static const Id& id;
    Id getId() const final {
        return id;
    }

    NamespaceString fromNss;
    std::string as;
    std::vector<BSONObj> pipeline;
    BSONObj letVariables;
    boost::optional<std::string> localField;
    boost::optional<std::string> foreignField;
    boost::optional<BSONObj> unwindSpec;
    bool hasForeignDB;

    // TODO SERVER-121094 This can be removed once hybrid search desugars into the internal hybrid
    // search stage.
    bool isHybridSearch;

    // The StageParams for each stage of the subpipeline. Present when $lookup includes a 'pipeline'
    // field (may be empty vector for explicit `pipeline: []`), and absent when $lookup uses the
    // local/foreignField-only form.
    boost::optional<StageParamsPipeline> subpipelineStageParams;

    // Position for the foreignField $match placeholder, set by the router when a
    // localField/foreignField+pipeline $lookup targets a view.
    boost::optional<int64_t> internalFieldMatchPipelineIdx;

    // Set by the router's $lookup rewrite when the original foreign namespace was a view
    // (including an identity view with `pipeline: []`). Used to restore _fromNsIsAView so $lookup
    // against a view is never SBE-lowered after the 'from' rewrite has erased the view name.
    bool internalFromIsAView = false;

    // True when the $lookup had no `pipeline:` field. Lets createFromStageParams keep _userPipeline
    // = boost::none instead of [], even after a view subpipeline is materialized.
    bool noUserPipeline = false;

    // The subpipeline's first (desugared) stage's view-application policy. kDefaultPrepend (the
    // default) means the stage is view-agnostic, so when 'fromNss' is a view the resolved view
    // pipeline is prepended ahead of the user subpipeline as usual. kDoNothing means the stage
    // applies the view itself (e.g. an extension search stage), so the view pipeline must not be
    // prepended to the resolved pipeline.
    FirstStageViewApplicationPolicy subpipelineViewPolicy =
        FirstStageViewApplicationPolicy::kDefaultPrepend;

private:
    // Owns the BSON buffer that DefaultStageParams::_originalSpec points into.
    BSONObj _ownedOriginalBson;
};

class LiteParsedLookUp final : public LiteParsedDocumentSourceNestedPipelines<LiteParsedLookUp> {
public:
    static constexpr std::string_view kStageName = "$lookup"sv;

    static std::unique_ptr<LiteParsedLookUp> parse(const NamespaceString& nss,
                                                   const BSONElement& spec,
                                                   const LiteParserOptions& options);

    LiteParsedLookUp(const BSONElement& spec,
                     NamespaceString foreignNss,
                     boost::optional<OwnedLiteParsedPipeline> pipeline,
                     std::vector<BSONObj> rawPipeline,
                     std::string as,
                     BSONObj letVariables,
                     boost::optional<std::string> localField,
                     boost::optional<std::string> foreignField,
                     boost::optional<BSONObj> unwindSpec,
                     bool hasForeignDB,
                     bool isHybridSearch,
                     boost::optional<int64_t> internalFieldMatchPipelineIdx = boost::none,
                     bool internalFromIsAView = false,
                     bool noUserPipeline = false,
                     bool allowGenericForeignDbLookup = false);

    Status checkShardedForeignCollAllowed(const NamespaceString& nss,
                                          bool inMultiDocumentTransaction) const final;

    void getForeignExecutionNamespaces(stdx::unordered_set<NamespaceString>& nssSet) const final;

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final;

    bool requiresAuthzChecks() const override {
        return false;
    }

    bool hasExtensionSearchStage() const override;

    std::unique_ptr<StageParams> getStageParams() const override;

    void validate() const override;

    // Moved from document_source_lookup.cpp so createFromBson can still call it.
    static void validateLookupCollectionlessPipeline(const std::vector<BSONObj>& pipeline);
    static void validateLookupCollectionlessPipeline(const BSONElement& pipelineElem);

protected:
    // $lookup materializes a view subpipeline only when it had no user `pipeline:` field.
    bool needsViewSubpipelineMaterialized() const override {
        return _noUserPipeline;
    }

private:
    std::vector<BSONObj> _rawPipeline;
    std::string _as;
    BSONObj _letVariables;
    boost::optional<std::string> _localField;
    boost::optional<std::string> _foreignField;
    boost::optional<BSONObj> _unwindSpec;
    bool _hasForeignDB;
    bool _isHybridSearch;

    // TODO SERVER-129127 This field can be removed when it lives on a ParseContext and is passed at
    // parse/validation time to the LiteParsedLookup.
    bool _allowGenericForeignDbLookup;

    // Parsed from $_internalFieldMatchPipelineIdx at construction so getStageParams() can forward
    // it.
    boost::optional<int64_t> _internalFieldMatchPipelineIdx;
    // Parsed from $_internalFromIsAView; forwarded by getStageParams() to restore _fromNsIsAView
    // on the shard.
    bool _internalFromIsAView = false;
    // True when there was no `pipeline:` field. Distinguishes this case from an explicit
    // `pipeline: []` so _userPipeline stays boost::none.
    bool _noUserPipeline = false;
};

}  // namespace mongo
