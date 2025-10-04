/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"

#include <utility>

namespace mongo {

/**
 * Base class of all GroupProcessor implementations. This class is used by the aggregation framework
 * and streams enterprise module to perform the document processing needed for $group.
 */
class GroupProcessorBase {
public:
    using Accumulators = std::vector<boost::intrusive_ptr<AccumulatorState>>;
    using GroupsMap = ValueUnorderedMap<Accumulators>;

    GroupProcessorBase(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       int64_t maxMemoryUsageBytes);

    GroupProcessorBase(GroupProcessorBase&& other) = default;
    GroupProcessorBase(const GroupProcessorBase& other)
        : _expCtx(other._expCtx),
          _idFieldNames(other._idFieldNames),
          _idExpressions(other._idExpressions),
          _accumulatedFields(other._accumulatedFields),
          _accumulatedFieldMemoryTrackers(other._accumulatedFieldMemoryTrackers),
          _doingMerge(other._doingMerge),
          _willBeMerged(other._willBeMerged),
          _memoryTracker(other._memoryTracker.makeFreshMemoryUsageTracker()),
          _executionStarted(other._executionStarted),
          _groups(other._groups),
          _stats(other._stats) {}

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
     * This must be called before the very first accumulate() call to indicate that the processor
     * object has been fully initialized.
     */
    void setExecutionStarted();

    /**
     * Returns the group key for the given document.
     */
    Value computeGroupKey(const Document& root) const;

    /**
     * Finds the group for the given key. Note that this method does not insert a new group when
     * the group does not already exist.
     */
    inline GroupsMap::iterator findGroup(const Value& key) {
        return _groups.find(key);
    }

    /**
     * Finds the group for the given key. Insert a new group when the group does not already exist.
     */
    std::pair<GroupsMap::iterator, bool> findOrCreateGroup(const Value& key);

    /**
     * Computes the argument for the accumulator at the given index for the given document.
     */
    inline Value computeAccumulatorArg(const Document& root, size_t accumulatorIdx) const {
        return _accumulatedFields.at(accumulatorIdx)
            .expr.argument->evaluate(root, &_expCtx->variables);
    }

    /**
     * Adds the given argument to the accumulator at the given index for the given group.
     */
    void accumulate(GroupsMap::iterator groupIter, size_t accumulatorIdx, Value accumulatorArg);

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
     * Returns true if this GroupProcessor is used by the shard part of a split $group which will be
     * merged together later.
     */
    bool willBeMerged() const {
        return _willBeMerged;
    }

    /**
     * Tell this object that it is the shard part of a split group, and the results will
     * be merged later.
     */
    void setWillBeMerged(bool willBeMerged) {
        _willBeMerged = willBeMerged;
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

protected:
    Document makeDocument(const Value& id, const Accumulators& accums);

    // Converts the internal representation of the group key to the _id shape specified by the
    // user.
    Value expandId(const Value& val);

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    // If the expression for the '_id' field represents a non-empty object, we track its fields'
    // names in '_idFieldNames'.
    std::vector<std::string> _idFieldNames;
    // Expressions for the individual fields when '_id' produces a document in the order of
    // '_idFieldNames' or the whole expression otherwise.
    std::vector<boost::intrusive_ptr<Expression>> _idExpressions;

    std::vector<AccumulationStatement> _accumulatedFields;
    // Per-field memory trackers corresponding to each AccumulationStatement in _accumulatedFields.
    // Caching these helps avoid lookups in the map in MemoryUsageTracker for every input document.
    std::vector<SimpleMemoryUsageTracker*> _accumulatedFieldMemoryTrackers;

    // Only set to true for a merging $group when a $group is split by distributedPlanLogic().
    bool _doingMerge{false};

    // Only set to true when a $group is split by distributedPlanLogic(), and only for the $group
    // pushed down to shards.
    bool _willBeMerged{false};

    MemoryUsageTracker _memoryTracker;

    // This flag should be set before the very first call to accumulate() to assert that accessor
    // methods that provide access to mutable member variables are not called during runtime.
    bool _executionStarted{false};

    GroupsMap _groups;
    GroupStats _stats;
};

}  // namespace mongo
