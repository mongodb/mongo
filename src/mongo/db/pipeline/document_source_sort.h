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

#include "mongo/db/exec/sort_executor.h"
#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/db/sorter/sorter.h"

namespace mongo {

class DocumentSourceSort final : public DocumentSource {
public:
    static constexpr StringData kMin = "min"_sd;
    static constexpr StringData kMax = "max"_sd;
    static constexpr StringData kOffset = "offsetSeconds"_sd;

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

    static constexpr StringData kStageName = "$sort"_sd;

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    void serializeToArray(
        std::vector<Value>& array,
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const final;

    GetModPathsReturn getModifiedPaths() const final {
        // A $sort does not modify any paths.
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
    }

    StageConstraints constraints(Pipeline::SplitState) const final {
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

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;
    bool canRunInParallelBeforeWriteStage(
        const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) const final;

    /**
     * Returns the sort key pattern.
     */
    const SortPattern& getSortKeyPattern() const {
        return _sortExecutor->sortPattern();
    }

    /**
     * Parses a $sort stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Creates a $sort stage. If maxMemoryUsageBytes is boost::none, then it will actually use the
     * value of 'internalQueryMaxBlockingSortMemoryUsageBytes'.
     */
    static boost::intrusive_ptr<DocumentSourceSort> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        const SortPattern& sortOrder,
        uint64_t limit = 0,
        boost::optional<uint64_t> maxMemoryUsageBytes = boost::none);

    /**
     * Convenience to create a $sort stage from BSON with no limit and the default memory limit.
     */
    static boost::intrusive_ptr<DocumentSourceSort> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx, const BSONObj& sortOrder) {
        return create(pExpCtx, {sortOrder, pExpCtx});
    }

    static boost::intrusive_ptr<DocumentSourceSort> createBoundedSort(
        SortPattern pat,
        StringData boundBase,
        long long boundOffset,
        boost::optional<long long> limit,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);
    /**
     * Parse a stage that uses BoundedSorter.
     */
    static boost::intrusive_ptr<DocumentSourceSort> parseBoundedSort(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Returns the the limit, if a subsequent $limit stage has been coalesced with this $sort stage.
     * Otherwise, returns boost::none.
     */
    boost::optional<long long> getLimit() const;

    /**
     * Loads a document to be sorted. This can be used to sort a stream of documents that are not
     * coming from another DocumentSource. Once all documents have been added, the caller must call
     * loadingDone() before using getNext() to receive the documents in sorted order.
     */
    void loadDocument(Document&& doc);

    /**
     * Signals to the sort stage that there will be no more input documents. It is an error to call
     * loadDocument() once this method returns.
     */
    void loadingDone();

    /**
     * Returns true if the sorter used disk while satisfying the query and false otherwise.
     */
    bool usedDisk() final;

    bool isPopulated() {
        return _populated;
    };

    bool isBoundedSortStage() {
        return (_timeSorter) ? true : false;
    }

    bool hasLimit() const {
        return _sortExecutor->hasLimit();
    }

    const SpecificStats* getSpecificStats() const final {
        return &_sortExecutor->stats();
    }

protected:
    GetNextResult doGetNext() final;
    /**
     * Attempts to absorb a subsequent $limit stage so that it can perform a top-k sort.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

private:
    DocumentSourceSort(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                       const SortPattern& sortOrder,
                       uint64_t limit,
                       uint64_t maxMemoryUsageBytes);

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        MONGO_UNREACHABLE;  // Should call serializeToArray instead.
    }

    /**
     * Before returning anything, we have to consume all input and sort it. This method consumes all
     * input and prepares the sorted stream '_output'.
     *
     * This method may not be able to finish populating the sorter in a single call if 'pSource'
     * returns a DocumentSource::GetNextResult::kPauseExecution, so it returns the last
     * GetNextResult encountered, which may be either kEOF or kPauseExecution.
     */
    GetNextResult populate();

    /**
     * Returns the sort key for 'doc', as well as the document that should be entered into the
     * sorter to eventually be returned. If we will need to later merge the sorted results with
     * other results, this method adds the sort key as metadata onto 'doc' to speed up the merge
     * later.
     */
    std::pair<Value, Document> extractSortKey(Document&& doc) const;

    /**
     * Returns the time value used to sort 'doc', as well as the document that should be entered
     * into the sorter to eventually be returned. If we will need to later merge the sorted results
     * with other results, this method adds the full sort key as metadata onto 'doc' to speed up the
     * merge later.
     */
    std::pair<Date_t, Document> extractTime(Document&& doc) const;

    /**
     * Peeks at the next document in the input. The next document is cached in _timeSorterNextDoc
     * to support peeking without advancing.
     */
    GetNextResult::ReturnStatus timeSorterPeek();

    /**
     * Peeks at the next document in the input, but ignores documents whose partition key differs
     * from the current partition key (if there is one).
     */
    GetNextResult::ReturnStatus timeSorterPeekSamePartition();

    /**
     * Gets the next document from the input. Caller must call timeSorterPeek() first, and it's
     * only valid to call timeSorterGetNext() if peek returned kAdvanced.
     */
    Document timeSorterGetNext();

    bool _populated = false;

    boost::optional<SortExecutor<Document>> _sortExecutor;

    boost::optional<SortKeyGenerator> _sortKeyGen;

    using TimeSorterInterface = BoundedSorterInterface<SortableDate, Document>;
    std::unique_ptr<TimeSorterInterface> _timeSorter;
    boost::optional<SortKeyGenerator> _timeSorterPartitionKeyGen;
    // The next document that will be returned by timeSorterGetNext().
    // timeSorterPeek() fills it in, and timeSorterGetNext() empties it.
    boost::optional<Document> _timeSorterNextDoc;
    // The current partition key.
    // If _timeSorterNextDoc has a document then this represents the partition key of
    // that document.
    // If _timeSorterNextDoc is empty then this represents the partition key of
    // the document last returned by timeSorterGetNext().
    boost::optional<Value> _timeSorterCurrentPartition;
    // Used in timeSorterPeek() to avoid calling getNext() on an exhausted pSource.
    bool _timeSorterInputEOF = false;

    QueryMetadataBitSet _requiredMetadata;
};

}  // namespace mongo
