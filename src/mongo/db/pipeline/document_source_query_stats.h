// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_stats/transform_algorithm_gen.h"
#include "mongo/db/tenant_id.h"
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
using namespace std::literals::string_view_literals;

using namespace query_stats;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(QueryStats);

class DocumentSourceQueryStats final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$queryStats"sv;

    class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options);

        LiteParsed(const BSONElement& spec,
                   const boost::optional<TenantId>& tenantId,
                   TransformAlgorithmEnum algorithm,
                   std::string hmacKey)
            : LiteParsedDocumentSourceDefault(spec),
              _algorithm(algorithm),
              _hmacKey(hmacKey),
              _privileges(
                  algorithm == TransformAlgorithmEnum::kNone
                      ? PrivilegeVector{Privilege(ResourcePattern::forClusterResource(tenantId),
                                                  ActionType::queryStatsReadTransformed),
                                        Privilege(ResourcePattern::forClusterResource(tenantId),
                                                  ActionType::queryStatsRead)}
                      : PrivilegeVector{Privilege(ResourcePattern::forClusterResource(tenantId),
                                                  ActionType::queryStatsReadTransformed)}) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return _privileges;
        }

        bool isInitialSource() const final {
            return true;
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(kStageName);
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<QueryStatsStageParams>(_originalBson);
        }

        const TransformAlgorithmEnum _algorithm;

        std::string _hmacKey;

        const PrivilegeVector _privileges;
    };

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    ~DocumentSourceQueryStats() override = default;

    StageConstraints constraints(PipelineSplitState = PipelineSplitState::kUnsplit) const override {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kReceivingHostOnly,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed};

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

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceQueryStatsToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    DocumentSourceQueryStats(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             TransformAlgorithmEnum algorithm = TransformAlgorithmEnum::kNone,
                             std::string hmacKey = {})
        : DocumentSource(kStageName, expCtx),
          _transformIdentifiers(algorithm != TransformAlgorithmEnum::kNone),
          _algorithm(algorithm),
          _hmacKey(hmacKey) {}

    // When true, apply hmac to field names from returned query shapes.
    bool _transformIdentifiers;

    // The type of algorithm to use for transform identifiers as an enum, currently only
    // kHmacSha256
    // ("hmac-sha-256") is supported.
    const TransformAlgorithmEnum _algorithm;

    /**
     * Key used for SHA-256 HMAC application on field names.
     */
    std::string _hmacKey;
};

}  // namespace mongo
