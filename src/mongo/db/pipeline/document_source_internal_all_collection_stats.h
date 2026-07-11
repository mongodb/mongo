// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_all_collection_stats_gen.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/unordered_set.h"
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

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(InternalAllCollectionStats);

/**
 * This aggregation stage is the '$_internalAllCollectionStats´. It takes no arguments. Its
 * response will be a cursor, each document of which represents the collection statistics for a
 * single collection for all the existing collections.
 *
 * When executing the '$_internalAllCollectionStats' aggregation stage, we will need to obtain the
 * catalog containing all collections namespaces.
 *
 * Then, for each collection, we will call `makeStatsForNs` method from DocumentSourceCollStats that
 * will retrieve all storage stats for that particular collection.
 */
class DocumentSourceInternalAllCollectionStats final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalAllCollectionStats"sv;

    DocumentSourceInternalAllCollectionStats(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                             DocumentSourceInternalAllCollectionStatsSpec spec);

    class LiteParsed final : public LiteParsedDocumentSourceDefault<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(nss.tenantId(), spec);
        }

        LiteParsed(const boost::optional<TenantId>& tenantId, const BSONElement& spec)
            : LiteParsedDocumentSourceDefault(spec),
              _privileges({Privilege(ResourcePattern::forClusterResource(tenantId),
                                     ActionType::allCollectionStats)}) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return _privileges;
        }

        bool isInitialSource() const final {
            return true;
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<InternalAllCollectionStatsStageParams>(_originalBson);
        }

    private:
        const PrivilegeVector _privileges;
    };

    std::string_view getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {};

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kTargetedShards,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);

        constraints.isIndependentOfAnyCollection = true;
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBsonInternal(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

    void serializeToArray(std::vector<Value>& array,
                          const query_shape::SerializationOptions& opts =
                              query_shape::SerializationOptions{}) const final;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalAllCollectionStatsToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    // The specification object given to $_internalAllCollectionStats containing user specified
    // options.
    const DocumentSourceInternalAllCollectionStatsSpec _internalAllCollectionStatsSpec;

    // A $match stage can be absorbed in order to avoid unnecessarily computing the stats for
    // collections that do not match that predicate.
    boost::intrusive_ptr<DocumentSourceMatch> _absorbedMatch;

    // If a $project stage exists after $_internalAllCollectionStats, we will peek the BSONObj
    // associated with the $project. This BSONObj will be used to avoid calculating
    // unnecessary fields.
    boost::optional<BSONObj> _projectFilter;
};
}  // namespace mongo
