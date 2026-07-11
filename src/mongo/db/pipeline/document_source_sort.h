// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_type_endian.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/sort_executor.h"
#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(Sort);
class SortLiteParsed final : public LiteParsedDocumentSourceDefault<SortLiteParsed> {
public:
    SortLiteParsed(const BSONElement& originalBson)
        : LiteParsedDocumentSourceDefault<SortLiteParsed>(originalBson) {}

    static std::unique_ptr<SortLiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {
        return std::make_unique<SortLiteParsed>(spec);
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<SortStageParams>(_originalBson);
    }

    // $sort is treated as a ranked stage for hybrid search validation purposes: a pipeline
    // containing $sort satisfies the "ranked pipeline" requirement of $rankFusion.
    bool isRankedStage() const final {
        return true;
    }

    // $sort only reorders documents without modifying them.
    bool isSelectionStage() const final {
        return true;
    }
};
DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(InternalBoundedSort);

class [[MONGO_MOD_NEEDS_REPLACEMENT]] DocumentSourceSort final : public DocumentSource {
public:
    static constexpr std::string_view kMin = "min"sv;
    static constexpr std::string_view kMax = "max"sv;
    static constexpr std::string_view kOffset = "offsetSeconds"sv;
    static constexpr std::string_view kInternalLimit = "$_internalLimit"sv;
    static constexpr std::string_view kInternalOutputSortKey = "$_internalOutputSortKeyMetadata"sv;

    struct SortStageOptions {
        uint64_t limit = 0;
        bool outputSortKeyMetadata = false;
    };

    static const SortStageOptions kDefaultOptions;

    struct SortableDate {
        Date_t date;

        struct SorterDeserializeSettings {};  // unused
        void serializeForSorter(BufBuilder& buf) const {
            buf.appendNum(date.toMillisSinceEpoch());
        }
        static SortableDate deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
            return {Date_t::fromMillisSinceEpoch(buf.read<LittleEndian<long long>>().value)};
        }
        int memUsageForSorter() const {
            return sizeof(SortableDate);
        }
        std::string toString() const {
            return date.toString();
        }
    };

    using TimeSorterInterface = BoundedSorterInterface<SortableDate, Document>;

    static constexpr std::string_view kStageName = "$sort"sv;

    /**
     * Parses a $sort stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&);

    /**
     * Creates a $sort stage. If maxMemoryUsageBytes is boost::none, then it will actually use the
     * value of 'internalQueryMaxBlockingSortMemoryUsageBytes'.
     */
    static boost::intrusive_ptr<DocumentSourceSort> create(
        const boost::intrusive_ptr<ExpressionContext>&,
        const SortPattern&,
        SortStageOptions options = kDefaultOptions);

    /**
     * Convenience to create a $sort stage from BSON with no limit and the default memory limit.
     */
    static boost::intrusive_ptr<DocumentSourceSort> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& sortOrder) {
        return create(expCtx, {sortOrder, expCtx}, kDefaultOptions);
    }

    // TODO SERVER-108133 Consider passing in SortStageOptions instead of limit and
    // outputSortKeyMetadata.
    static boost::intrusive_ptr<DocumentSourceSort> createBoundedSort(
        SortPattern pat,
        std::string_view boundBase,
        long long boundOffset,
        boost::optional<long long> limit,
        bool outputSortKeyMetadata,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Parse a stage that uses BoundedSorter.
     */
    static boost::intrusive_ptr<DocumentSourceSort> parseBoundedSort(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&);

    /**
     * The constructor.
     */
    DocumentSourceSort(const boost::intrusive_ptr<ExpressionContext>&,
                       const SortPattern&,
                       SortStageOptions);

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    void serializeToArray(std::vector<Value>& array,
                          const query_shape::SerializationOptions& opts =
                              query_shape::SerializationOptions{}) const final;

    GetModPathsReturn getModifiedPaths() const final {
        // A $sort does not modify any paths.
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
    }

    /**
     * Requests that this stage should output the sort key metadata with each result.
     */
    void pleaseOutputSortKeyMetadata() {
        _outputSortKeyMetadata = true;
    }

    bool providesSortKeyMetadata() const override {
        // TODO SERVER-98624 It would be preferable to just set '_outputSortKeyMetadata' based on
        // 'getNeedsMerge()' in the constructor or some earlier time. Sadly, we can't do this right
        // now without adding complexity elsewhere to account for mixed-version clusters. If you set
        // '_outputSortKeyMetadata' to true, then it will possibly mean serializing a new field when
        // sending a $sort to another node in the cluster (as of the time of this writing). This is
        // OK today because the callers who set this option during construction first must check the
        // FCV (and/or a feature flag), which guards against mixed-version scenarios.
        return _outputSortKeyMetadata || getExpCtx()->getNeedsMerge();
    }

    StageConstraints constraints(PipelineSplitState) const final {
        StageConstraints constraints(StreamType::kBlocking,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kWritesTmpData,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed,
                                     ChangeStreamRequirement::kDenylist);
        constraints.preservesCardinality = true;
        // Can't swap with a $match if a limit has been absorbed, as $match can't swap with $limit.
        constraints.canSwapWithMatch = !_sortExecutor->hasLimit();
        return constraints;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final;

    bool canRunInParallelBeforeWriteStage(
        const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) const final;

    const std::shared_ptr<SortExecutor<Document>>& getSortExecutor() const {
        return _sortExecutor;
    }

    /**
     * Returns the the limit, if a subsequent $limit stage has been coalesced with this $sort stage.
     * Otherwise, returns boost::none.
     */
    boost::optional<long long> getLimit() const;

    bool isBoundedSortStage() const {
        return bool(_timeSorter);
    }

    bool hasLimit() const {
        return _sortExecutor->hasLimit();
    }

    /**
     * Attempts to absorb a subsequent $limit stage so that it can perform a top-k sort.
     */
    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceSortToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    Value serialize(const query_shape::SerializationOptions& opts) const final {
        MONGO_UNREACHABLE_TASSERT(7484302);  // Should call serializeToArray instead.
    }

    /**
     * Helper functions used by serializeToArray() to serialize this stage.
     */
    void serializeForBoundedSort(std::vector<Value>& array,
                                 const query_shape::SerializationOptions& opts) const;
    void serializeWithVerbosity(std::vector<Value>& array,
                                const query_shape::SerializationOptions& opts) const;
    void serializeForCloning(std::vector<Value>& array,
                             const query_shape::SerializationOptions& opts) const;

    QueryMetadataBitSet _requiredMetadata;

    std::shared_ptr<SortExecutor<Document>> _sortExecutor;
    std::shared_ptr<TimeSorterInterface> _timeSorter;
    // TODO: SERVER-105521 This member can be moved instead of shared.
    std::shared_ptr<SortKeyGenerator> _timeSorterPartitionKeyGen;

    // Whether to include metadata including the sort key in the output documents from this stage.
    bool _outputSortKeyMetadata = false;

    template <typename TimeSorter>
    friend auto makeSorter(const ExpressionContext& expCtx,
                           const DocumentSourceSort& ds,
                           const SortOptions& opts,
                           long long boundOffset);
};

}  // namespace mongo
