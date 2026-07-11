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

#include <list>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace [[MONGO_MOD_NEEDS_REPLACEMENT]] mongo {
using namespace std::literals::string_view_literals;

class UnionWithStageParams : public DefaultStageParams {
public:
    UnionWithStageParams(NamespaceString unionNss,
                         std::vector<BSONObj> pipeline,
                         bool isHybridSearch,
                         BSONObj ownedBsonObj,
                         boost::optional<StageParamsPipeline> subpipelineStageParams = boost::none,
                         ResolvedNamespace resolvedBackingNss = {})
        : DefaultStageParams(ownedBsonObj.firstElement()),
          unionNss(std::move(unionNss)),
          pipeline(std::move(pipeline)),
          isHybridSearch(isHybridSearch),
          subpipelineStageParams(std::move(subpipelineStageParams)),
          resolvedBackingNss(std::move(resolvedBackingNss)),
          _ownedOriginalBson(std::move(ownedBsonObj)) {}

    static const Id& id;
    Id getId() const final {
        return id;
    }

    NamespaceString unionNss;
    std::vector<BSONObj> pipeline;

    // TODO SERVER-121094 This can be removed once hybrid search desugars into the internal hybrid
    // search stage.
    bool isHybridSearch;

    // The StageParams for each stage of the subpipeline. Absent only when a $unionWith runs with no
    // user pipeline specified against a collection (non-view).
    boost::optional<StageParamsPipeline> subpipelineStageParams;

    // The resolved backing namespace the subpipeline targets, populated by
    // LiteParsedDocumentSourceNestedPipelines::bindResolvedNamespace at parse time. Identity
    // (not-a-view) unless a view was stitched; check `isInvolvedNamespaceAView()`.
    ResolvedNamespace resolvedBackingNss;

private:
    // Owns the BSON buffer that DefaultStageParams::_originalSpec points into.
    BSONObj _ownedOriginalBson;
};

class LiteParsedUnionWith final
    : public LiteParsedDocumentSourceNestedPipelines<LiteParsedUnionWith> {
public:
    static constexpr std::string_view kStageName = "$unionWith"sv;

    static std::unique_ptr<LiteParsedUnionWith> parse(const NamespaceString& nss,
                                                      const BSONElement& spec,
                                                      const LiteParserOptions& options);

    LiteParsedUnionWith(const BSONElement& spec,
                        NamespaceString foreignNss,
                        boost::optional<OwnedLiteParsedPipeline> pipeline,
                        std::vector<BSONObj> rawPipeline,
                        bool isHybridSearch);

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final;

    bool requiresAuthzChecks() const override;

    std::unique_ptr<StageParams> getStageParams() const override;

    bool hasExtensionVectorSearchStage() const override;

    bool hasExtensionSearchStage() const override;

    static void validateUnionWithCollectionlessPipeline(
        const boost::optional<std::vector<mongo::BSONObj>>& pipeline);

protected:
    bool needsViewSubpipelineMaterialized() const override {
        return _pipelines.empty();
    }

private:
    std::vector<BSONObj> _rawPipeline;
    bool _isHybridSearch;
};

}  // namespace mongo
