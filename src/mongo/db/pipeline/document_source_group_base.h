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

#include <memory>
#include <utility>

#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/group_from_first_document_transformation.h"
#include "mongo/db/pipeline/memory_usage_tracker.h"
#include "mongo/db/sorter/sorter.h"

namespace mongo {

/**
 * This class represents a $group stage generically - could be a streaming or hash based group.
 *
 * It contains some common execution code between the two algorithms, such as:
 *  - Handling spilling to disk.
 *  - Computing the group key
 *  - Accumulating values and populating output documents.
 */
class DocumentSourceGroupBase : public DocumentSource {
public:
    using Accumulators = std::vector<boost::intrusive_ptr<AccumulatorState>>;
    using GroupsMap = ValueUnorderedMap<Accumulators>;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;
    boost::intrusive_ptr<DocumentSource> optimize() final;
    DepsTracker::State getDependencies(DepsTracker* deps) const final;
    void addVariableRefs(std::set<Variables::Id>* refs) const final;
    GetModPathsReturn getModifiedPaths() const final;
    StringMap<boost::intrusive_ptr<Expression>> getIdFields() const;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;

    /**
     * Can be used to change or swap out individual _id fields, but should not be used
     * once execution has begun.
     */
    std::vector<boost::intrusive_ptr<Expression>>& getMutableIdFields();
    const std::vector<AccumulationStatement>& getAccumulatedFields() const;

    /**
     * Can be used to change or swap out individual accumulated fields, but should not be used
     * once execution has begun.
     */
    std::vector<AccumulationStatement>& getMutableAccumulatedFields();

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints(StreamType::kBlocking,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kWritesTmpData,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);
        constraints.canSwapWithMatch = true;
        return constraints;
    }

    /**
     * Add an accumulator, which will become a field in each Document that results from grouping.
     */
    void addAccumulator(AccumulationStatement accumulationStatement);

    /**
     * Sets the expression to use to determine the group id of each document.
     */
    void setIdExpression(boost::intrusive_ptr<Expression> idExpression);

    /**
     * Returns the expression to use to determine the group id of each document.
     */
    boost::intrusive_ptr<Expression> getIdExpression() const;

    /**
     * Returns true if this $group stage represents a 'global' $group which is merging together
     * results from earlier partial groups.
     */
    bool doingMerge() const {
        return _doingMerge;
    }

    /**
     * Tell this source if it is doing a merge from shards. Defaults to false.
     */
    void setDoingMerge(bool doingMerge) {
        _doingMerge = doingMerge;
    }

    /**
     * Returns true if this $group stage used disk during execution and false otherwise.
     */
    bool usedDisk() final {
        return _stats.spills > 0;
    }

    const SpecificStats* getSpecificStats() const final {
        return &_stats;
    }

    bool canRunInParallelBeforeWriteStage(
        const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) const final;

    /**
     * When possible, creates a document transformer that transforms the first document in a group
     * into one of the output documents of the $group stage. This is possible when we are grouping
     * on a single field and all accumulators are $first (or there are no accumluators).
     *
     * It is sometimes possible to use a DISTINCT_SCAN to scan the first document of each group,
     * in which case this transformation can replace the actual $group stage in the pipeline
     * (SERVER-9507).
     */
    std::unique_ptr<GroupFromFirstDocumentTransformation> rewriteGroupAsTransformOnFirstDocument()
        const;

    /**
     * Returns maximum allowed memory footprint.
     */
    size_t getMaxMemoryUsageBytes() const;

    // True if this $group can be pushed down to SBE.
    bool sbeCompatible() const {
        return _sbeCompatible;
    }

protected:
    DocumentSourceGroupBase(StringData stageName,
                            const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            boost::optional<size_t> maxMemoryUsageBytes = boost::none);

    void initializeFromBson(BSONElement elem);
    virtual bool isSpecFieldReserved(StringData fieldName) = 0;

    void doDispose() final;

    /**
     * Cleans up any pending memory usage. Throws error, if memory usage is above
     * 'maxMemoryUsageBytes' and cannot spill to disk.
     *
     * Returns true, if the caller should spill to disk, false otherwise.
     */
    bool shouldSpillWithAttemptToSaveMemory();

    /**
     * Spill groups map to disk and returns an iterator to the file. Note: Since a sorted $group
     * does not exhaust the previous stage before returning, and thus does not maintain as large a
     * store of documents at any one time, only an unsorted group can spill to disk.
     */
    void spill();

    /**
     * Computes the internal representation of the group key.
     */
    Value computeId(const Document& root);

    void processDocument(const Value& id, const Document& root);

    void readyGroups();
    void resetReadyGroups();

    GetNextResult getNextReadyGroup();

    void setExecutionStarted() {
        _executionStarted = true;
    }

    virtual void serializeAdditionalFields(
        MutableDocument& out, boost::optional<ExplainOptions::Verbosity> explain) const {};

    // If the expression for the '_id' field represents a non-empty object, we track its fields'
    // names in '_idFieldNames'.
    std::vector<std::string> _idFieldNames;
    // Expressions for the individual fields when '_id' produces a document in the order of
    // '_idFieldNames' or the whole expression otherwise.
    std::vector<boost::intrusive_ptr<Expression>> _idExpressions;

private:
    GetNextResult getNextSpilled();
    GetNextResult getNextStandard();

    /**
     * If we ran out of memory, finish all the pending operations so that some memory
     * can be freed.
     */
    void freeMemory();

    Document makeDocument(const Value& id, const Accumulators& accums, bool mergeableOutput);

    /**
     * Converts the internal representation of the group key to the _id shape specified by the
     * user.
     */
    Value expandId(const Value& val);

    /**
     * Returns true if 'dottedPath' is one of the group keys present in '_idExpressions'.
     */
    bool pathIncludedInGroupKeys(const std::string& dottedPath) const;

    std::vector<AccumulationStatement> _accumulatedFields;

    bool _doingMerge;

    MemoryUsageTracker _memoryTracker;

    GroupStats _stats;

    /**
     * This flag should be set during first execution of getNext() to assert that non-const methods
     * that expose internal structures are not called during runtime.
     */
    bool _executionStarted;

    // We use boost::optional to defer initialization until the ExpressionContext containing the
    // correct comparator is injected, since the groups must be built using the comparator's
    // definition of equality.
    boost::optional<GroupsMap> _groups;

    // Tracks the size of the spill file.
    std::unique_ptr<SorterFileStats> _spillStats;
    std::shared_ptr<Sorter<Value, Value>::File> _file;
    std::vector<std::shared_ptr<Sorter<Value, Value>::Iterator>> _sortedFiles;
    bool _spilled;


    // Only used when '_spilled' is false.
    GroupsMap::iterator _groupsIterator;

    // Only used when '_spilled' is true.
    std::unique_ptr<Sorter<Value, Value>::Iterator> _sorterIterator;

    std::pair<Value, Value> _firstPartOfNextGroup;
    Accumulators _currentAccumulators;

    bool _sbeCompatible;
};

}  // namespace mongo
