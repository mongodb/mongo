// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(Limit);
class LimitLiteParsed final : public LiteParsedDocumentSourceDefault<LimitLiteParsed> {
public:
    LimitLiteParsed(const BSONElement& originalBson)
        : LiteParsedDocumentSourceDefault<LimitLiteParsed>(originalBson) {}

    static std::unique_ptr<LimitLiteParsed> parse(const NamespaceString& nss,
                                                  const BSONElement& spec,
                                                  const LiteParserOptions& options) {
        return std::make_unique<LimitLiteParsed>(spec);
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<LimitStageParams>(_originalBson);
    }

    // $limit only reduces the number of documents without modifying them.
    bool isSelectionStage() const final {
        return true;
    }
};

class [[MONGO_MOD_NEEDS_REPLACEMENT]] DocumentSourceLimit final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$limit"sv;

    /**
     * Create a new $limit stage.
     */
    static boost::intrusive_ptr<DocumentSourceLimit> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long limit);

    /**
     * Parse a $limit stage from a BSON stage specification. 'elem's field name must be "$limit".
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        return StageConstraints{StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kAllowed,
                                LookupRequirement::kAllowed,
                                UnionRequirement::kAllowed};
    }

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    /**
     * Attempts to combine with a subsequent $limit stage, setting 'limit' appropriately.
     */
    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);
    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::SEE_NEXT;  // This doesn't affect needed fields
    }

    /**
     * Returns a DistributedPlanLogic with two identical $limit stages; one for the shards pipeline
     * and one for the merging pipeline.
     */
    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        // Running this stage on the shards is an optimization, but is not strictly necessary in
        // order to produce correct pipeline output.
        // {shardsStage, mergingStage, sortPattern}
        return DistributedPlanLogic{
            this, DocumentSourceLimit::create(getExpCtx(), _limit), boost::none};
    }

    long long getLimit() const {
        return _limit;
    }
    void setLimit(long long newLimit) {
        _limit = newLimit;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    DocumentSourceLimit(const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long limit);

    long long _limit;
};

}  // namespace mongo
