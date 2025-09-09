/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/data_type_endian.h"
#include "mongo/base/string_data.h"
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
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class DocumentSourceSort final : public DocumentSource {
public:
    static constexpr StringData kMin = "min"_sd;
    static constexpr StringData kMax = "max"_sd;
    static constexpr StringData kOffset = "offsetSeconds"_sd;
    static constexpr StringData kInternalLimit = "$_internalLimit"_sd;
    static constexpr StringData kInternalOutputSortKey = "$_internalOutputSortKeyMetadata"_sd;

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

    static constexpr StringData kStageName = "$sort"_sd;

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
        StringData boundBase,
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

    const char* getSourceName() const final {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    void serializeToArray(std::vector<Value>& array,
                          const SerializationOptions& opts = SerializationOptions{}) const final;

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

    /**
     * Returns true if the output documents of this $sort stage are supposed to have the sort key
     * metadata field populated.
     */
    bool shouldSetSortKeyMetadata() const {
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

        // Can't swap with a $match if a limit has been absorbed, as $match can't swap with $limit.
        constraints.canSwapWithMatch = !_sortExecutor->hasLimit();
        return constraints;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;

    bool canRunInParallelBeforeWriteStage(
        const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) const final;

    const std::shared_ptr<SortExecutor<Document>>& getSortExecutor() const {
        return _sortExecutor;
    }

    /**
     * Returns the sort key pattern.
     */
    const SortPattern& getSortKeyPattern() const {
        return _sortExecutor->sortPattern();
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

protected:
    /**
     * Attempts to absorb a subsequent $limit stage so that it can perform a top-k sort.
     */
    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceSortToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    Value serialize(const SerializationOptions& opts) const final {
        MONGO_UNREACHABLE_TASSERT(7484302);  // Should call serializeToArray instead.
    }

    /**
     * Helper functions used by serializeToArray() to serialize this stage.
     */
    void serializeForBoundedSort(std::vector<Value>& array, const SerializationOptions& opts) const;
    void serializeWithVerbosity(std::vector<Value>& array, const SerializationOptions& opts) const;
    void serializeForCloning(std::vector<Value>& array, const SerializationOptions& opts) const;

    QueryMetadataBitSet _requiredMetadata;

    std::shared_ptr<SortExecutor<Document>> _sortExecutor;
    std::shared_ptr<TimeSorterInterface> _timeSorter;
    // TODO: SERVER-105521 This member can be moved instead of shared.
    std::shared_ptr<SortKeyGenerator> _timeSorterPartitionKeyGen;

    // Whether to include metadata including the sort key in the output documents from this stage.
    bool _outputSortKeyMetadata = false;
};

}  // namespace mongo
