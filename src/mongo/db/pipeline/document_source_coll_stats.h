// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_coll_stats_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(CollStats);

/**
 * Provides a document source interface to retrieve collection-level statistics for a given
 * collection.
 */
class DocumentSourceCollStats : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$collStats"sv;

    class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& specElem,
                                                 const LiteParserOptions& options) {
            uassert(5447000,
                    str::stream() << "$collStats must take a nested object but found: " << specElem,
                    specElem.type() == BSONType::object);
            auto spec = DocumentSourceCollStatsSpec::parse(specElem.embeddedObject(),
                                                           IDLParserContext(kStageName));
            return std::make_unique<LiteParsed>(specElem, nss, std::move(spec));
        }

        LiteParsed(const BSONElement& specElem,
                   NamespaceString nss,
                   DocumentSourceCollStatsSpec spec)
            : LiteParsedDocumentSourceDefault(specElem),
              _nss(std::move(nss)),
              _spec(std::move(spec)) {}

        bool isCollStats() const final {
            return true;
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return {Privilege(ResourcePattern::forExactNamespace(_nss), ActionType::collStats)};
        }

        void assertPermittedInAPIVersion(const APIParameters&) const override;

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<CollStatsStageParams>(_originalBson);
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        bool isInitialSource() const final {
            return true;
        }

    private:
        const NamespaceString _nss;
        const DocumentSourceCollStatsSpec _spec;
    };

    DocumentSourceCollStats(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                            DocumentSourceCollStatsSpec spec)
        : DocumentSource(kStageName, pExpCtx),
          _collStatsSpec(std::move(spec)),
          _targetAllNodes(_collStatsSpec.getTargetAllNodes().value_or(false)) {}

    std::string_view getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        HostTypeRequirement hostTypeRequirement = _targetAllNodes
            ? HostTypeRequirement::kAllShardHosts
            : HostTypeRequirement::kTargetedShards;
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     hostTypeRequirement,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);

        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const override;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceCollStatsToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    // The raw object given to $collStats containing user specified options.
    DocumentSourceCollStatsSpec _collStatsSpec;
    bool _targetAllNodes;
};

}  // namespace mongo
