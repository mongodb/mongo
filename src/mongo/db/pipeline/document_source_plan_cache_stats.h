// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(PlanCacheStats);

class DocumentSourcePlanCacheStats final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$planCacheStats"sv;

    class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(spec, nss);
        }

        LiteParsed(const BSONElement& spec, NamespaceString nss)
            : LiteParsedDocumentSourceDefault(spec), _nss(std::move(nss)) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            // There are no foreign collections.
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return {Privilege(ResourcePattern::forExactNamespace(_nss), ActionType::planCacheRead)};
        }

        bool isInitialSource() const final {
            return true;
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<PlanCacheStatsStageParams>(_originalBson);
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            return onlyReadConcernLocalSupported(kStageName, level, isImplicitDefault);
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(DocumentSourcePlanCacheStats::kStageName);
        }

    private:
        const NamespaceString _nss;
    };

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    ~DocumentSourcePlanCacheStats() override = default;

    StageConstraints constraints(PipelineSplitState = PipelineSplitState::kUnsplit) const override {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     _allHosts ? HostTypeRequirement::kAllShardHosts
                                               : HostTypeRequirement::kTargetedShards,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed};

        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    std::string_view getSourceName() const override {
        return DocumentSourcePlanCacheStats::kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    /**
     * Absorbs a subsequent $match, in order to avoid copying the entire contents of the plan cache
     * prior to filtering.
     */
    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

    void serializeToArray(std::vector<Value>& array,
                          const query_shape::SerializationOptions& opts =
                              query_shape::SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourcePlanCacheStatsToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    DocumentSourcePlanCacheStats(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 bool allHosts);

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final {
        MONGO_UNREACHABLE_TASSERT(7484303);  // Should call serializeToArray instead.
    }

    // If true, requests plan cache stats from all data-bearing nodes, primary and secondary.
    // Otherwise, follows read preference.
    const bool _allHosts;

    // $planCacheStats can push a match down into the plan cache layer, in order to avoid copying
    // the entire contents of the cache.
    boost::intrusive_ptr<DocumentSourceMatch> _absorbedMatch;
};

}  // namespace mongo
