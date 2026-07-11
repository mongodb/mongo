// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_list_sampled_queries_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

// Forward declaration needed due to compiler error caused by namespace difference.
boost::intrusive_ptr<exec::agg::Stage> documentSourceListSampledQueriesToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource);

namespace analyze_shard_key {
using namespace std::literals::string_view_literals;

// TODO: SERVER-105521 Remove this struct as both the '_pipeline' and '_execPipeline' are used only
// in ListSampledQueriesStage (assuming 'detachSourceFromOperationContext()' and
// 'reattachSourceToOperationContext()' will be removed as well).
struct ListSampledQueriesSharedState {
    std::unique_ptr<Pipeline> pipeline;
    std::unique_ptr<exec::agg::Pipeline> execPipeline;
};

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ListSampledQueries);

class DocumentSourceListSampledQueries final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$listSampledQueries"sv;

    class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& specElem,
                                                 const LiteParserOptions& options);

        LiteParsed(const BSONElement& specElem,
                   NamespaceString nss,
                   DocumentSourceListSampledQueriesSpec spec)
            : LiteParsedDocumentSourceDefault(specElem),
              _nss(std::move(nss)),
              _privileges({Privilege(ResourcePattern::forClusterResource(_nss.tenantId()),
                                     ActionType::listSampledQueries)}) {}

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return _privileges;
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        bool isInitialSource() const final {
            return true;
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<ListSampledQueriesStageParams>(_originalBson);
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(kStageName);
        }

    private:
        const NamespaceString _nss;
        const PrivilegeVector _privileges;
    };

    DocumentSourceListSampledQueries(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                     DocumentSourceListSampledQueriesSpec spec)
        : DocumentSource(kStageName, pExpCtx),
          _spec(std::move(spec)),
          _sharedState(std::make_shared<ListSampledQueriesSharedState>()) {}

    ~DocumentSourceListSampledQueries() override = default;

    StageConstraints constraints(PipelineSplitState = PipelineSplitState::kUnsplit) const override {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed};

        constraints.isIndependentOfAnyCollection = true;
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    std::string_view getSourceName() const override {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    void detachSourceFromOperationContext() final;
    void reattachSourceToOperationContext(OperationContext* opCtx) final;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> mongo::documentSourceListSampledQueriesToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    DocumentSourceListSampledQueries(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(kStageName, expCtx),
          _sharedState(std::make_shared<ListSampledQueriesSharedState>()) {};

    DocumentSourceListSampledQueriesSpec _spec;

    std::shared_ptr<ListSampledQueriesSharedState> _sharedState;
};

}  // namespace analyze_shard_key
}  // namespace mongo
