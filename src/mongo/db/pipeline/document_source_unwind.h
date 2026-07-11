// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/agg/unwind_processor.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_internal.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

// TODO SERVER-116044: Remove external dependencies on this header.
namespace [[MONGO_MOD_NEEDS_REPLACEMENT]] mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(Unwind);

class DocumentSourceUnwind final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$unwind"sv;

    // virtuals from DocumentSource
    std::string_view getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    /**
     * Returns the unwound path, and the 'includeArrayIndex' path, if specified.
     */
    GetModPathsReturn getModifiedPaths() const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);
        constraints.canSwapWithMatch = true;
        constraints.outputDependsOnSingleInput = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    /**
     * Creates a new $unwind DocumentSource from a BSON specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSourceUnwind> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const std::string& path,
        bool includeNullIfEmptyOrMissing,
        const boost::optional<std::string>& includeArrayIndex,
        bool strict = false);

    const std::string& getUnwindPath() const {
        return _unwindPath.fullPath();
    }

    bool preserveNullAndEmptyArrays() const {
        return _preserveNullAndEmptyArrays;
    }

    const boost::optional<FieldPath>& indexPath() const {
        return _indexPath;
    }

    SbeCompatibility sbeCompatibility() const {
        return _sbeCompatibility;
    }

    /**
     * Attempts to swap with a subsequent $sort stage if the $sort is on a different field.
     */
    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

private:
    friend std::unique_ptr<exec::agg::UnwindProcessor> createUnwindProcessorFromDocumentSource(
        const boost::intrusive_ptr<DocumentSourceUnwind>& ds);

    DocumentSourceUnwind(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         const FieldPath& fieldPath,
                         bool includeNullIfEmptyOrMissing,
                         const boost::optional<FieldPath>& includeArrayIndex,
                         bool strict);

    // Checks if a sort is eligible to be moved before the unwind.
    bool canPushSortBack(const DocumentSourceSort* sort) const;

    // Checks if a limit is eligible to be moved before the unwind.
    bool canPushLimitBack(const DocumentSourceLimit* limit) const;

    // 'UnwindProcessor' constructor args.
    const FieldPath _unwindPath;
    bool _preserveNullAndEmptyArrays;
    const boost::optional<FieldPath> _indexPath;
    bool _strict;

    // If preserveNullAndEmptyArrays is true and unwind is followed by a limit, we can duplicate
    // the limit before the unwind. We only want to do this if we've found a limit smaller than the
    // one we already pushed down. boost::none means no push down has occurred yet.
    boost::optional<long long> _smallestLimitPushedDown;

    // Standalone $unwind pushdown to SBE requires featureFlagSbeFull.
    SbeCompatibility _sbeCompatibility{SbeCompatibility::requiresSbeFull};
};  // class DocumentSourceUnwind

}  // namespace mongo
