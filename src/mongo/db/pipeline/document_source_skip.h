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

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(Skip);
class SkipLiteParsed final : public LiteParsedDocumentSourceDefault<SkipLiteParsed> {
public:
    SkipLiteParsed(const BSONElement& originalBson)
        : LiteParsedDocumentSourceDefault<SkipLiteParsed>(originalBson) {}

    static std::unique_ptr<SkipLiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {
        return std::make_unique<SkipLiteParsed>(spec);
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<SkipStageParams>(_originalBson);
    }

    // $skip only skips documents without modifying them.
    bool isSelectionStage() const final {
        return true;
    }
};

class DocumentSourceSkip final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$skip"sv;

    /**
     * Convenience method for creating a $skip stage.
     */
    static boost::intrusive_ptr<DocumentSourceSkip> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long nToSkip);

    /**
     * Parses the user-supplied BSON into a $skip stage.
     *
     * Throws a AssertionException if 'elem' is an invalid $skip specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        return {StreamType::kStreaming,
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
     * Attempts to move a subsequent $limit before the skip, potentially allowing for forther
     * optimizations earlier in the pipeline.
     */
    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    boost::intrusive_ptr<DocumentSource> optimize();

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::SEE_NEXT;  // This doesn't affect needed fields
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    /**
     * The $skip stage must run on the merging half of the pipeline.
     */
    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        // {shardsStage, mergingStage, sortPattern}
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    long long getSkip() const {
        return _nToSkip;
    }
    void setSkip(long long newSkip) {
        _nToSkip = newSkip;
    }

private:
    explicit DocumentSourceSkip(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                long long nToSkip);

    long long _nToSkip = 0;
};

}  // namespace mongo
