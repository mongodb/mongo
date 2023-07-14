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

#include "mongo/db/pipeline/group_processor.h"

#include <memory>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/stats/counters.h"

namespace mongo {

namespace {

/**
 * Generates a new file name on each call using a static, atomic and monotonically increasing
 * number.
 *
 * Each user of the Sorter must implement this function to ensure that all temporary files that the
 * Sorter instances produce are uniquely identified using a unique file name extension with separate
 * atomic variable. This is necessary because the sorter.cpp code is separately included in multiple
 * places, rather than compiled in one place and linked, and so cannot provide a globally unique ID.
 */
std::string nextFileName() {
    static AtomicWord<unsigned> documentSourceGroupFileCounter;
    return "extsort-doc-group." + std::to_string(documentSourceGroupFileCounter.fetchAndAdd(1));
}

}  // namespace

GroupProcessor::GroupProcessor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               boost::optional<size_t> maxMemoryUsageBytes)
    : _expCtx(expCtx),
      _memoryTracker{expCtx->allowDiskUse && !expCtx->inMongos,
                     maxMemoryUsageBytes
                         ? *maxMemoryUsageBytes
                         : static_cast<size_t>(internalDocumentSourceGroupMaxMemoryBytes.load())},
      _groups(expCtx->getValueComparator().makeUnorderedValueMap<Accumulators>()) {}

void GroupProcessor::addAccumulationStatement(AccumulationStatement accumulationStatement) {
    tassert(7801002, "Can't mutate accumulated fields after initialization", !_executionStarted);
    _accumulatedFields.push_back(accumulationStatement);
    _memoryTracker.set(accumulationStatement.fieldName, 0);
}

void GroupProcessor::freeMemory() {
    for (auto& group : _groups) {
        for (size_t i = 0; i < group.second.size(); ++i) {
            // Subtract the current usage.
            _memoryTracker.update(_accumulatedFields[i].fieldName,
                                  -1 * group.second[i]->getMemUsage());

            group.second[i]->reduceMemoryConsumptionIfAble();

            // Update the memory usage for this AccumulationStatement.
            _memoryTracker.update(_accumulatedFields[i].fieldName, group.second[i]->getMemUsage());
        }
    }
}

boost::optional<Document> GroupProcessor::getNext() {
    if (_spilled) {
        return getNextSpilled();
    } else {
        return getNextStandard();
    }
}

boost::optional<Document> GroupProcessor::getNextSpilled() {
    // We aren't streaming, and we have spilled to disk.
    if (!_sorterIterator)
        return boost::none;

    Value currentId = _firstPartOfNextGroup.first;
    const size_t numAccumulators = _accumulatedFields.size();

    // Call startNewGroup on every accumulator.
    Value expandedId = expandId(currentId);
    Document idDoc =
        expandedId.getType() == BSONType::Object ? expandedId.getDocument() : Document();
    for (size_t i = 0; i < numAccumulators; ++i) {
        Value initializerValue =
            _accumulatedFields[i].expr.initializer->evaluate(idDoc, &_expCtx->variables);
        _currentAccumulators[i]->reset();
        _currentAccumulators[i]->startNewGroup(initializerValue);
    }

    while (_expCtx->getValueComparator().evaluate(currentId == _firstPartOfNextGroup.first)) {
        // Inside of this loop, _firstPartOfNextGroup is the current data being processed.
        // At loop exit, it is the first value to be processed in the next group.
        switch (numAccumulators) {  // mirrors switch in spill()
            case 1:                 // Single accumulators serialize as a single Value.
                _currentAccumulators[0]->process(_firstPartOfNextGroup.second, true);
                [[fallthrough]];
            case 0:  // No accumulators so no Values.
                break;
            default: {  // Multiple accumulators serialize as an array of Values.
                const std::vector<Value>& accumulatorStates =
                    _firstPartOfNextGroup.second.getArray();
                for (size_t i = 0; i < numAccumulators; i++) {
                    _currentAccumulators[i]->process(accumulatorStates[i], true);
                }
            }
        }

        if (!_sorterIterator->more()) {
            _sorterIterator.reset();
            break;
        }

        _firstPartOfNextGroup = _sorterIterator->next();
    }

    return makeDocument(currentId, _currentAccumulators, _expCtx->needsMerge);
}

boost::optional<Document> GroupProcessor::getNextStandard() {
    // Not spilled, and not streaming.
    if (_groupsIterator == _groups.end())
        return boost::none;

    Document out =
        makeDocument(_groupsIterator->first, _groupsIterator->second, _expCtx->needsMerge);
    ++_groupsIterator;
    return out;
}

void GroupProcessor::setIdExpression(const boost::intrusive_ptr<Expression> idExpression) {
    tassert(7801001, "Can't mutate _id fields after initialization", !_executionStarted);
    if (auto object = dynamic_cast<ExpressionObject*>(idExpression.get())) {
        auto& childExpressions = object->getChildExpressions();
        invariant(!childExpressions.empty());  // We expect to have converted an empty object into a
                                               // constant expression.

        // grouping on an "artificial" object. Rather than create the object for each input
        // in initialize(), instead group on the output of the raw expressions. The artificial
        // object will be created at the end in makeDocument() while outputting results.
        for (auto&& childExpPair : childExpressions) {
            _idFieldNames.push_back(childExpPair.first);
            _idExpressions.push_back(childExpPair.second);
        }
    } else {
        _idExpressions.push_back(idExpression);
    }
}

boost::intrusive_ptr<Expression> GroupProcessor::getIdExpression() const {
    // _idFieldNames is empty and _idExpressions has one element when the _id expression is not an
    // object expression.
    if (_idFieldNames.empty() && _idExpressions.size() == 1) {
        return _idExpressions[0];
    }

    tassert(6586300,
            "Field and its expression must be always paired in ExpressionObject",
            _idFieldNames.size() > 0 && _idFieldNames.size() == _idExpressions.size());

    // Each expression in '_idExpressions' may have been optimized and so, compose the object _id
    // expression out of the optimized expressions.
    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> fieldsAndExprs;
    for (size_t i = 0; i < _idExpressions.size(); ++i) {
        fieldsAndExprs.emplace_back(_idFieldNames[i], _idExpressions[i]);
    }

    return ExpressionObject::create(_idExpressions[0]->getExpressionContext(),
                                    std::move(fieldsAndExprs));
}

namespace {

using GroupsMap = GroupProcessor::GroupsMap;

class SorterComparator {
public:
    SorterComparator(ValueComparator valueComparator) : _valueComparator(valueComparator) {}

    int operator()(const Value& lhs, const Value& rhs) const {
        return _valueComparator.compare(lhs, rhs);
    }

private:
    ValueComparator _valueComparator;
};

class SpillSTLComparator {
public:
    SpillSTLComparator(ValueComparator valueComparator) : _valueComparator(valueComparator) {}

    bool operator()(const GroupsMap::value_type* lhs, const GroupsMap::value_type* rhs) const {
        return _valueComparator.evaluate(lhs->first < rhs->first);
    }

private:
    ValueComparator _valueComparator;
};

}  // namespace

void GroupProcessor::add(const Value& id, const Document& root) {
    const size_t numAccumulators = _accumulatedFields.size();

    // Look for the _id value in the map. If it's not there, add a new entry with a blank
    // accumulator. This is done in a somewhat odd way in order to avoid hashing 'id' and
    // looking it up in '_groups' multiple times.
    const size_t oldSize = _groups.size();
    std::vector<boost::intrusive_ptr<AccumulatorState>>& group = _groups[id];
    const bool inserted = _groups.size() != oldSize;

    std::vector<uint64_t> oldAccumMemUsage(numAccumulators, 0);
    if (inserted) {
        _memoryTracker.set(_memoryTracker.currentMemoryBytes() + id.getApproximateSize());

        // Initialize and add the accumulators
        Value expandedId = expandId(id);
        Document idDoc =
            expandedId.getType() == BSONType::Object ? expandedId.getDocument() : Document();
        group.reserve(numAccumulators);
        for (auto& accumulatedField : _accumulatedFields) {
            auto accum = accumulatedField.makeAccumulator();
            Value initializerValue =
                accumulatedField.expr.initializer->evaluate(idDoc, &_expCtx->variables);
            accum->startNewGroup(initializerValue);
            group.push_back(std::move(accum));
        }
    }

    // Check that we have accumulated state for each of the accumulation statements.
    dassert(numAccumulators == group.size());

    for (size_t i = 0; i < numAccumulators; i++) {
        // Only process the input and update the memory footprint if the current accumulator
        // needs more input.
        if (group[i]->needsInput()) {
            const auto prevMemUsage = inserted ? 0 : group[i]->getMemUsage();
            group[i]->process(
                _accumulatedFields[i].expr.argument->evaluate(root, &_expCtx->variables),
                _doingMerge);
            _memoryTracker.update(_accumulatedFields[i].fieldName,
                                  group[i]->getMemUsage() - prevMemUsage);
        }
    }

    if (shouldSpillWithAttemptToSaveMemory() || shouldSpillForDebugBuild(inserted)) {
        spill();
    }
}

void GroupProcessor::readyGroups() {
    _spilled = !_sortedFiles.empty();
    if (_spilled) {
        if (!_groups.empty()) {
            spill();
        }

        _groups = _expCtx->getValueComparator().makeUnorderedValueMap<Accumulators>();

        _sorterIterator.reset(Sorter<Value, Value>::Iterator::merge(
            _sortedFiles, SortOptions(), SorterComparator(_expCtx->getValueComparator())));

        // prepare current to accumulate data
        _currentAccumulators.reserve(_accumulatedFields.size());
        for (auto&& accumulatedField : _accumulatedFields) {
            _currentAccumulators.push_back(accumulatedField.makeAccumulator());
        }

        MONGO_verify(_sorterIterator->more());  // we put data in, we should get something out.
        _firstPartOfNextGroup = _sorterIterator->next();
    } else {
        // start the group iterator
        _groupsIterator = _groups.begin();
    }
}

void GroupProcessor::reset() {
    // Free our resources.
    _groups = _expCtx->getValueComparator().makeUnorderedValueMap<Accumulators>();
    _memoryTracker.resetCurrent();
    _sorterIterator.reset();
    _sortedFiles.clear();

    // Make us look done.
    _groupsIterator = _groups.end();
}

Value GroupProcessor::computeId(const Document& root) const {
    // If only one expression, return result directly
    if (_idExpressions.size() == 1) {
        Value retValue = _idExpressions[0]->evaluate(root, &_expCtx->variables);
        return retValue.missing() ? Value(BSONNULL) : std::move(retValue);
    }

    // Multiple expressions get results wrapped in a vector
    std::vector<Value> vals;
    vals.reserve(_idExpressions.size());
    for (size_t i = 0; i < _idExpressions.size(); i++) {
        vals.push_back(_idExpressions[i]->evaluate(root, &_expCtx->variables));
    }
    return Value(std::move(vals));
}

Value GroupProcessor::expandId(const Value& val) {
    // _id doesn't get wrapped in a document
    if (_idFieldNames.empty())
        return val;

    // _id is a single-field document containing val
    if (_idFieldNames.size() == 1)
        return Value(DOC(_idFieldNames[0] << val));

    // _id is a multi-field document containing the elements of val
    const std::vector<Value>& vals = val.getArray();
    invariant(_idFieldNames.size() == vals.size());
    MutableDocument md(vals.size());
    for (size_t i = 0; i < vals.size(); i++) {
        md[_idFieldNames[i]] = vals[i];
    }
    return md.freezeToValue();
}

Document GroupProcessor::makeDocument(const Value& id,
                                      const Accumulators& accums,
                                      bool mergeableOutput) {
    const size_t n = _accumulatedFields.size();
    MutableDocument out(1 + n);

    /* add the _id field */
    out.addField("_id", expandId(id));

    /* add the rest of the fields */
    for (size_t i = 0; i < n; ++i) {
        Value val = accums[i]->getValue(mergeableOutput);
        if (val.missing()) {
            // we return null in this case so return objects are predictable
            out.addField(_accumulatedFields[i].fieldName, Value(BSONNULL));
        } else {
            out.addField(_accumulatedFields[i].fieldName, std::move(val));
        }
    }

    _stats.totalOutputDataSizeBytes += out.getApproximateSize();
    return out.freeze();
}

bool GroupProcessor::shouldSpillWithAttemptToSaveMemory() {
    if (!_memoryTracker._allowDiskUse && !_memoryTracker.withinMemoryLimit()) {
        freeMemory();
    }

    if (!_memoryTracker.withinMemoryLimit()) {
        uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
                "Exceeded memory limit for $group, but didn't allow external sort."
                " Pass allowDiskUse:true to opt in.",
                _memoryTracker._allowDiskUse);
        return true;
    }
    return false;
}

bool GroupProcessor::shouldSpillForDebugBuild(bool isNewGroup) {
    // In debug mode, spill every time we have a duplicate id to stress merge logic.
    return (kDebugBuild && !_expCtx->opCtx->readOnly() && !isNewGroup &&  // is not a new group
            !_expCtx->inMongos &&            // can't spill to disk in mongos
            _memoryTracker._allowDiskUse &&  // never spill when disk use is explicitly prohibited
            _sortedFiles.size() < 20);
}

void GroupProcessor::spill() {
    _stats.spills++;
    _stats.numBytesSpilledEstimate += _memoryTracker.currentMemoryBytes();
    _stats.spilledRecords += _groups.size();

    std::vector<const GroupsMap::value_type*> ptrs;  // using pointers to speed sorting
    ptrs.reserve(_groups.size());
    for (GroupsMap::const_iterator it = _groups.begin(), end = _groups.end(); it != end; ++it) {
        ptrs.push_back(&*it);
    }

    stable_sort(ptrs.begin(), ptrs.end(), SpillSTLComparator(_expCtx->getValueComparator()));

    // Initialize '_file' in a lazy manner only when it is needed.
    if (!_file) {
        _spillStats = std::make_unique<SorterFileStats>(nullptr /* sorterTracker */);
        _file = std::make_shared<Sorter<Value, Value>::File>(
            _expCtx->tempDir + "/" + nextFileName(), _spillStats.get());
    }
    SortedFileWriter<Value, Value> writer(SortOptions().TempDir(_expCtx->tempDir), _file);
    switch (_accumulatedFields.size()) {  // same as ptrs[i]->second.size() for all i.
        case 0:                           // no values, essentially a distinct
            for (size_t i = 0; i < ptrs.size(); i++) {
                writer.addAlreadySorted(ptrs[i]->first, Value());
            }
            break;

        case 1:  // just one value, use optimized serialization as single Value
            for (size_t i = 0; i < ptrs.size(); i++) {
                writer.addAlreadySorted(ptrs[i]->first,
                                        ptrs[i]->second[0]->getValue(/*toBeMerged=*/true));
            }
            break;

        default:  // multiple values, serialize as array-typed Value
            for (size_t i = 0; i < ptrs.size(); i++) {
                std::vector<Value> accums;
                for (size_t j = 0; j < ptrs[i]->second.size(); j++) {
                    accums.push_back(ptrs[i]->second[j]->getValue(/*toBeMerged=*/true));
                }
                writer.addAlreadySorted(ptrs[i]->first, Value(std::move(accums)));
            }
            break;
    }

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_expCtx->opCtx);
    metricsCollector.incrementKeysSorted(ptrs.size());
    metricsCollector.incrementSorterSpills(1);

    _groups.clear();
    // Zero out the current per-accumulation statement memory consumption, as the memory has been
    // freed by spilling.
    _memoryTracker.resetCurrent();

    _sortedFiles.emplace_back(writer.done());
    if (_spillStats) {
        _stats.spilledDataStorageSize = _spillStats->bytesSpilled();
    }
}

}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
// Explicit instantiation unneeded since we aren't exposing Sorter outside of this file.
