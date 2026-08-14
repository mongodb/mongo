// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(JoinPlanCacheStats);

/**
 * The $joinPlanCacheStats aggregation stage dumps the contents of the node-global join plan cache.
 * Unlike $planCacheStats (which reads a per-collection cache), the join plan cache is a single
 * ServiceContext-decorated cache, so this stage is a collectionless initial source run as
 * {aggregate: 1} against the 'admin' database. It requires join optimization and the join plan
 * cache to be enabled via server parameters.
 *
 * In a sharded cluster each shard maintains its own independent join plan cache, so the router
 * forwards the stage to every shard and unions the resulting cursors; there is no merging stage.
 * Entries returned through a router carry an additional 'shard' field naming the shard they came
 * from.
 *
 * The stage fully desugars at parse time into a 'DocumentSourceQueue' seeded with a deferred
 * lambda, so it has no DocumentSource or exec stage implementation of its own. See
 * createFromBson().
 */
class DocumentSourceJoinPlanCacheStats final {
public:
    static constexpr std::string_view kStageName = "$joinPlanCacheStats"sv;

    class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(spec, nss);
        }

        LiteParsed(const BSONElement& spec, const NamespaceString& nss)
            : LiteParsedDocumentSourceDefault(spec),
              _privileges({Privilege(ResourcePattern::forDatabaseName(nss.dbName()),
                                     ActionType::planCacheRead)}) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            // There are no foreign collections.
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return _privileges;
        }

        bool isInitialSource() const final {
            return true;
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<JoinPlanCacheStatsStageParams>(_originalBson);
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            return onlyReadConcernLocalSupported(kStageName, level, isImplicitDefault);
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(DocumentSourceJoinPlanCacheStats::kStageName);
        }

    private:
        const PrivilegeVector _privileges;
    };

    /**
     * The constraints reported by the 'DocumentSourceQueue' backing this stage. 'kTargetedShards'
     * is what sends the stage to every shard so each one dumps its own join plan cache; the
     * 'DocumentSourceQueue' defaults ('kCollectionlessSourceRunOnceAnyNode', transactions and
     * $lookup/$unionWith allowed) would be wrong here.
     */
    static StageConstraints constraints() {
        StageConstraints constraints{StageConstraints::StreamType::kStreaming,
                                     StageConstraints::PositionRequirement::kFirst,
                                     StageConstraints::HostTypeRequirement::kTargetedShards,
                                     StageConstraints::DiskUseRequirement::kNoDiskUse,
                                     StageConstraints::FacetRequirement::kNotAllowed,
                                     StageConstraints::TransactionRequirement::kNotAllowed,
                                     StageConstraints::LookupRequirement::kNotAllowed,
                                     StageConstraints::UnionRequirement::kNotAllowed};

        constraints.isIndependentOfAnyCollection = true;
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    // It is illegal to construct a DocumentSourceJoinPlanCacheStats directly, use createFromBson()
    // instead.
    DocumentSourceJoinPlanCacheStats() = default;
};

}  // namespace mongo
