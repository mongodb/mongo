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
#include "mongo/db/pipeline/group_from_first_document_transformation.h"
#include "mongo/db/pipeline/memory_usage_tracker.h"
#include "mongo/db/sorter/sorter.h"

namespace mongo {

/**
 * This class is used by the aggregation framework and streams enterprise module to perform the
 * document processing needed for $group.
 *
 * A caller should call the public methods of this class in the following manner:
 * - For a document, call computeId() to compute its group key and then add the document to the
 *   processor using the add() method. Do this for every input document.
 * - Once all documents are added to the processor, call readyGroups() to indicate that there are no
 *   more documents to add.
 * - Repeatedly call getNext() to get all aggregated result documents.
 * - Eventually call reset() to reset the processor to its original state.
 */
class GroupProcessor {
public:
    using Accumulators = std::vector<boost::intrusive_ptr<AccumulatorState>>;
    using GroupsMap = ValueUnorderedMap<Accumulators>;

    GroupProcessor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                   boost::optional<size_t> maxMemoryUsageBytes = boost::none);

    /**
     * Sets the expression to use to determine the group id of each document.
     * This must be called before setExecutionStarted().
     */
    void setIdExpression(boost::intrusive_ptr<Expression> idExpression);

    /**
     * Add an AccumulationStatement, which will become a field in each Document that results from
     * grouping. This must be called before setExecutionStarted().
     */
    void addAccumulationStatement(AccumulationStatement accumulationStatement);

    /**
     * This must be called before the very first add() call to indicate that the processor object
     * has been fully initialized.
     */
    void setExecutionStarted() {
        _executionStarted = true;
    }

    /**
     * Computes the internal representation of the group key.
     */
    Value computeId(const Document& root) const;

    /**
     * Adds the given document to the group corresponding to the specified group key.
     */
    void add(const Value& id, const Document& root);

    /**
     * Prepares internal state to start returning fully aggregated groups back to the caller via
     * getNext() calls. Note that add() must not be called after this method is called.
     */
    void readyGroups();

    /**
     * Returns the next aggregated result document. Returns boost::none if there are no more
     * documents to return.
     *
     * Note that this must be called after readyGroups() has already been called once.
     */
    boost::optional<Document> getNext();

    /**
     * Resets the internal state to match the initial state.
     */
    void reset();

    /**
     * Returns the field names, if any, used to determine the group id of each document.
     * This vector is non-empty only when the expression sent to setIdExpression() is an
     * ExpressionObject. This vector then contains the field names of fields used to construct this
     * object.
     */
    const std::vector<std::string>& getIdFieldNames() const {
        return _idFieldNames;
    }

    /**
     * Returns the expression used to determine the group id of each document.
     * When the expression sent to setIdExpression() is an ExpressionObject, the returned vector
     * contains the expressions used to compute the individual values in this object.
     */
    const std::vector<boost::intrusive_ptr<Expression>>& getIdExpressions() const {
        return _idExpressions;
    }

    /**
     * Similar to above, but can be used to change or swap out individual id expressions.
     * Should not be used once execution has begun.
     */
    std::vector<boost::intrusive_ptr<Expression>>& getMutableIdExpressions() {
        tassert(7020503, "Can't mutate _id fields after initialization", !_executionStarted);
        return _idExpressions;
    }

    /**
     * Returns all the AccumulationStatements added via addAccumulationStatement().
     */
    const std::vector<AccumulationStatement>& getAccumulationStatements() const {
        return _accumulatedFields;
    }

    /**
     * Similar to above, but can be used to change or swap out individual accumulated fields.
     * Should not be used once execution has begun.
     */
    std::vector<AccumulationStatement>& getMutableAccumulationStatements() {
        tassert(
            7020504, "Can't mutate accumulated fields after initialization", !_executionStarted);
        return _accumulatedFields;
    }

    /**
     * Returns the expression to use to determine the group id of each document.
     */
    boost::intrusive_ptr<Expression> getIdExpression() const;

    /**
     * Returns true if this GroupProcessor is used by a 'global' $group which is merging together
     * results from earlier partial groups.
     */
    bool doingMerge() const {
        return _doingMerge;
    }

    /**
     * Tell this object if it is doing a merge from shards. Defaults to false.
     */
    void setDoingMerge(bool doingMerge) {
        _doingMerge = doingMerge;
    }

    /**
     * Returns true if this GroupProcessor stage used disk during execution and false otherwise.
     */
    bool usedDisk() const {
        return _stats.spills > 0;
    }

    const GroupStats& getStats() const {
        return _stats;
    }

    const MemoryUsageTracker& getMemoryTracker() const {
        return _memoryTracker;
    }

    /**
     * If we ran out of memory, finish all the pending operations so that some memory
     * can be freed.
     */
    void freeMemory();

private:
    boost::optional<Document> getNextSpilled();
    boost::optional<Document> getNextStandard();

    Document makeDocument(const Value& id, const Accumulators& accums, bool mergeableOutput);

    /**
     * Converts the internal representation of the group key to the _id shape specified by the
     * user.
     */
    Value expandId(const Value& val);

    /**
     * Cleans up any pending memory usage. Throws error, if memory usage is above
     * 'maxMemoryUsageBytes' and cannot spill to disk.
     *
     * Returns true, if the caller should spill to disk, false otherwise.
     */
    bool shouldSpillWithAttemptToSaveMemory();

    /**
     * Returns true if the caller should spill to disk in debug mode. Returns false otherwise.
     */
    bool shouldSpillForDebugBuild(bool isNewGroup);

    /**
     * Spills the GroupsMap to a new file and empties the map so that subsequent groups can be added
     * to it. Later when the groups need to be returned back to the caller, all groups in all the
     * spilled files are read, merged and returned to the caller.
     */
    void spill();

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    // If the expression for the '_id' field represents a non-empty object, we track its fields'
    // names in '_idFieldNames'.
    std::vector<std::string> _idFieldNames;
    // Expressions for the individual fields when '_id' produces a document in the order of
    // '_idFieldNames' or the whole expression otherwise.
    std::vector<boost::intrusive_ptr<Expression>> _idExpressions;

    std::vector<AccumulationStatement> _accumulatedFields;

    bool _doingMerge{false};

    MemoryUsageTracker _memoryTracker;
    GroupStats _stats;

    /**
     * This flag should be set before the very first call to add() to assert that accessor methods
     * that provide access to mutable member variables are not called during runtime.
     */
    bool _executionStarted{false};

    GroupsMap _groups;
    // Only used when '_spilled' is false.
    GroupsMap::iterator _groupsIterator;

    // Tracks the size of the spill file.
    std::unique_ptr<SorterFileStats> _spillStats;
    std::shared_ptr<Sorter<Value, Value>::File> _file;
    std::vector<std::shared_ptr<Sorter<Value, Value>::Iterator>> _sortedFiles;
    bool _spilled{false};

    // Only used when '_spilled' is true.
    std::unique_ptr<Sorter<Value, Value>::Iterator> _sorterIterator;
    std::pair<Value, Value> _firstPartOfNextGroup;
    Accumulators _currentAccumulators;
};

}  // namespace mongo
