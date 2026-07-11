// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/document_source_analyze_shard_key_read_write_distribution_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace analyze_shard_key {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(AnalyzeShardKeyReadWriteDistribution);

class DocumentSourceAnalyzeShardKeyReadWriteDistribution final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_analyzeShardKeyReadWriteDistribution"sv;

    class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& specElem,
                                                 const LiteParserOptions& options);

        explicit LiteParsed(const BSONElement& specElem,
                            NamespaceString nss,
                            DocumentSourceAnalyzeShardKeyReadWriteDistributionSpec spec)
            : LiteParsedDocumentSourceDefault(specElem), _nss(std::move(nss)) {}

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return {
                Privilege(ResourcePattern::forExactNamespace(_nss), ActionType::analyzeShardKey)};
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        bool isInitialSource() const final {
            return true;
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(kStageName);
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<AnalyzeShardKeyReadWriteDistributionStageParams>(_originalBson);
        }

    private:
        const NamespaceString _nss;
    };

    DocumentSourceAnalyzeShardKeyReadWriteDistribution(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        DocumentSourceAnalyzeShardKeyReadWriteDistributionSpec spec)
        : DocumentSource(kStageName, pExpCtx), _spec(std::move(spec)) {}

    ~DocumentSourceAnalyzeShardKeyReadWriteDistribution() override = default;

    StageConstraints constraints(PipelineSplitState = PipelineSplitState::kUnsplit) const override {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kTargetedShards,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed};

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

    auto& getSpec() const {
        return _spec;
    }

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceAnalyzeShardKeyReadWriteDistribution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(kStageName, expCtx) {}

    DocumentSourceAnalyzeShardKeyReadWriteDistributionSpec _spec;
};

}  // namespace analyze_shard_key
}  // namespace mongo
