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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_group_base.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/util/destructor_guard.h"

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

/**
 * Helper to check if all accumulated fields need the same document.
 */
bool accsNeedSameDoc(const std::vector<AccumulationStatement>& accumulatedFields,
                     AccumulatorDocumentsNeeded docNeeded) {
    return std::all_of(accumulatedFields.begin(), accumulatedFields.end(), [&](auto&& accumulator) {
        const auto& doc = accumulator.makeAccumulator()->documentsNeeded();
        return doc == docNeeded;
    });
}

}  // namespace

using boost::intrusive_ptr;
using std::pair;
using std::shared_ptr;
using std::vector;

DocumentSourceGroupBase::~DocumentSourceGroupBase() {
    groupCounters.incrementGroupCounters(
        _stats.spills, _stats.spilledDataStorageSize, _stats.spilledRecords);
}

Value DocumentSourceGroupBase::serialize(SerializationOptions opts) const {
    MutableDocument insides;

    // Add the _id.
    if (_idFieldNames.empty()) {
        invariant(_idExpressions.size() == 1);
        insides["_id"] = _idExpressions[0]->serialize(opts);
    } else {
        // Decomposed document case.
        invariant(_idExpressions.size() == _idFieldNames.size());
        MutableDocument md;
        for (size_t i = 0; i < _idExpressions.size(); i++) {
            md[opts.serializeFieldPathFromString(_idFieldNames[i])] =
                _idExpressions[i]->serialize(opts);
        }
        insides["_id"] = md.freezeToValue();
    }

    // Add the remaining fields.
    for (auto&& accumulatedField : _accumulatedFields) {
        intrusive_ptr<AccumulatorState> accum = accumulatedField.makeAccumulator();
        insides[opts.serializeFieldPathFromString(accumulatedField.fieldName)] =
            Value(accum->serialize(
                accumulatedField.expr.initializer, accumulatedField.expr.argument, opts));
    }

    if (_doingMerge) {
        insides["$doingMerge"] = opts.serializeLiteralValue(true);
    }

    serializeAdditionalFields(insides, opts);

    MutableDocument out;
    out[getSourceName()] = insides.freezeToValue();

    if (opts.verbosity && *opts.verbosity >= ExplainOptions::Verbosity::kExecStats) {
        MutableDocument md;

        for (size_t i = 0; i < _accumulatedFields.size(); i++) {
            md[opts.serializeFieldPathFromString(_accumulatedFields[i].fieldName)] =
                opts.serializeLiteralValue(static_cast<long long>(
                    _memoryTracker[_accumulatedFields[i].fieldName].maxMemoryBytes()));
        }

        out["maxAccumulatorMemoryUsageBytes"] = Value(md.freezeToValue());
        out["totalOutputDataSizeBytes"] =
            opts.serializeLiteralValue(static_cast<long long>(_stats.totalOutputDataSizeBytes));
        out["usedDisk"] = opts.serializeLiteralValue(_stats.spills > 0);
        out["spills"] = opts.serializeLiteralValue(static_cast<long long>(_stats.spills));
        out["spilledDataStorageSize"] =
            opts.serializeLiteralValue(static_cast<long long>(_stats.spilledDataStorageSize));
        out["numBytesSpilledEstimate"] =
            opts.serializeLiteralValue(static_cast<long long>(_stats.numBytesSpilledEstimate));
        out["spilledRecords"] =
            opts.serializeLiteralValue(static_cast<long long>(_stats.spilledRecords));
    }

    return out.freezeToValue();
}


bool DocumentSourceGroupBase::shouldSpillWithAttemptToSaveMemory() {
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

void DocumentSourceGroupBase::freeMemory() {
    invariant(_groups);
    for (auto&& group : *_groups) {
        for (size_t i = 0; i < group.second.size(); i++) {
            // Subtract the current usage.
            _memoryTracker.update(_accumulatedFields[i].fieldName,
                                  -1 * group.second[i]->getMemUsage());

            group.second[i]->reduceMemoryConsumptionIfAble();

            // Update the memory usage for this AccumulationStatement.
            _memoryTracker.update(_accumulatedFields[i].fieldName, group.second[i]->getMemUsage());
        }
    }
}

DocumentSource::GetNextResult DocumentSourceGroupBase::getNextReadyGroup() {
    if (_spilled) {
        return getNextSpilled();
    } else {
        return getNextStandard();
    }
}

DocumentSource::GetNextResult DocumentSourceGroupBase::getNextSpilled() {
    // We aren't streaming, and we have spilled to disk.
    if (!_sorterIterator)
        return GetNextResult::makeEOF();

    Value currentId = _firstPartOfNextGroup.first;
    const size_t numAccumulators = _accumulatedFields.size();

    // Call startNewGroup on every accumulator.
    Value expandedId = expandId(currentId);
    Document idDoc =
        expandedId.getType() == BSONType::Object ? expandedId.getDocument() : Document();
    for (size_t i = 0; i < numAccumulators; ++i) {
        Value initializerValue =
            _accumulatedFields[i].expr.initializer->evaluate(idDoc, &pExpCtx->variables);
        _currentAccumulators[i]->reset();
        _currentAccumulators[i]->startNewGroup(initializerValue);
    }

    while (pExpCtx->getValueComparator().evaluate(currentId == _firstPartOfNextGroup.first)) {
        // Inside of this loop, _firstPartOfNextGroup is the current data being processed.
        // At loop exit, it is the first value to be processed in the next group.
        switch (numAccumulators) {  // mirrors switch in spill()
            case 1:                 // Single accumulators serialize as a single Value.
                _currentAccumulators[0]->process(_firstPartOfNextGroup.second, true);
                [[fallthrough]];
            case 0:  // No accumulators so no Values.
                break;
            default: {  // Multiple accumulators serialize as an array of Values.
                const vector<Value>& accumulatorStates = _firstPartOfNextGroup.second.getArray();
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

    return makeDocument(currentId, _currentAccumulators, pExpCtx->needsMerge);
}

DocumentSource::GetNextResult DocumentSourceGroupBase::getNextStandard() {
    // Not spilled, and not streaming.
    if (_groupsIterator == _groups->end())
        return GetNextResult::makeEOF();

    Document out =
        makeDocument(_groupsIterator->first, _groupsIterator->second, pExpCtx->needsMerge);
    ++_groupsIterator;
    return out;
}

void DocumentSourceGroupBase::doDispose() {
    resetReadyGroups();
}

intrusive_ptr<DocumentSource> DocumentSourceGroupBase::optimize() {
    // Optimizing a 'DocumentSourceGroupBase' might modify its expressions to become incompatible
    // with SBE. We temporarily highjack the context's 'sbeCompatible' flag to communicate the
    // situation back to the 'DocumentSourceGroupBase'. Notice, that while a particular
    // 'DocumentSourceGroupBase' might become incompatible with SBE, other groups in the pipeline
    // and the collection access could be still eligible for lowering to SBE, thus we must reset the
    // context's 'sbeCompatible' flag back to its original value at the end of the 'optimize()'
    // call.
    //
    // TODO SERVER-XXXXX: replace this hack with a proper per-stage tracking of SBE compatibility.
    auto expCtx = _idExpressions[0]->getExpressionContext();
    auto origSbeCompatibility = expCtx->sbeCompatibility;
    expCtx->sbeCompatibility = SbeCompatibility::fullyCompatible;

    // TODO: If all _idExpressions are ExpressionConstants after optimization, then we know there
    // will be only one group. We should take advantage of that to avoid going through the hash
    // table.
    for (size_t i = 0; i < _idExpressions.size(); i++) {
        _idExpressions[i] = _idExpressions[i]->optimize();
    }

    for (auto&& accumulatedField : _accumulatedFields) {
        accumulatedField.expr.initializer = accumulatedField.expr.initializer->optimize();
        accumulatedField.expr.argument = accumulatedField.expr.argument->optimize();
    }

    _sbeCompatibility = std::min(_sbeCompatibility, expCtx->sbeCompatibility);
    expCtx->sbeCompatibility = origSbeCompatibility;

    return this;
}

DepsTracker::State DocumentSourceGroupBase::getDependencies(DepsTracker* deps) const {
    // add the _id
    for (size_t i = 0; i < _idExpressions.size(); i++) {
        expression::addDependencies(_idExpressions[i].get(), deps);
    }

    // add the rest
    for (auto&& accumulatedField : _accumulatedFields) {
        expression::addDependencies(accumulatedField.expr.argument.get(), deps);
        // Don't add initializer, because it doesn't refer to docs from the input stream.
    }

    return DepsTracker::State::EXHAUSTIVE_ALL;
}

void DocumentSourceGroupBase::addVariableRefs(std::set<Variables::Id>* refs) const {
    for (const auto& idExpr : _idExpressions) {
        expression::addVariableRefs(idExpr.get(), refs);
    }

    for (auto&& accumulatedField : _accumulatedFields) {
        expression::addVariableRefs(accumulatedField.expr.argument.get(), refs);
    }
}

DocumentSource::GetModPathsReturn DocumentSourceGroupBase::getModifiedPaths() const {
    // We preserve none of the fields, but any fields referenced as part of the group key are
    // logically just renamed.
    StringMap<std::string> renames;
    for (std::size_t i = 0; i < _idExpressions.size(); ++i) {
        auto idExp = _idExpressions[i];
        auto pathToPutResultOfExpression =
            _idFieldNames.empty() ? "_id" : "_id." + _idFieldNames[i];
        auto computedPaths = idExp->getComputedPaths(pathToPutResultOfExpression);
        for (auto&& rename : computedPaths.renames) {
            renames[rename.first] = rename.second;
        }
    }

    return {DocumentSource::GetModPathsReturn::Type::kAllExcept,
            OrderedPathSet{},  // No fields are preserved.
            std::move(renames)};
}

StringMap<boost::intrusive_ptr<Expression>> DocumentSourceGroupBase::getIdFields() const {
    if (_idFieldNames.empty()) {
        invariant(_idExpressions.size() == 1);
        return {{"_id", _idExpressions[0]}};
    } else {
        invariant(_idFieldNames.size() == _idExpressions.size());
        StringMap<boost::intrusive_ptr<Expression>> result;
        for (std::size_t i = 0; i < _idFieldNames.size(); ++i) {
            result["_id." + _idFieldNames[i]] = _idExpressions[i];
        }
        return result;
    }
}

std::vector<boost::intrusive_ptr<Expression>>& DocumentSourceGroupBase::getMutableIdFields() {
    tassert(7020503, "Can't mutate _id fields after initialization", !_executionStarted);
    return _idExpressions;
}

const std::vector<AccumulationStatement>& DocumentSourceGroupBase::getAccumulatedFields() const {
    return _accumulatedFields;
}

std::vector<AccumulationStatement>& DocumentSourceGroupBase::getMutableAccumulatedFields() {
    tassert(7020504, "Can't mutate accumulated fields after initialization", !_executionStarted);
    return _accumulatedFields;
}

DocumentSourceGroupBase::DocumentSourceGroupBase(StringData stageName,
                                                 const intrusive_ptr<ExpressionContext>& expCtx,
                                                 boost::optional<size_t> maxMemoryUsageBytes)
    : DocumentSource(stageName, expCtx),
      _doingMerge(false),
      _memoryTracker{expCtx->allowDiskUse && !expCtx->inMongos,
                     maxMemoryUsageBytes
                         ? *maxMemoryUsageBytes
                         : static_cast<size_t>(internalDocumentSourceGroupMaxMemoryBytes.load())},
      _executionStarted(false),
      _groups(expCtx->getValueComparator().makeUnorderedValueMap<Accumulators>()),
      _spilled(false),
      _sbeCompatibility(SbeCompatibility::notCompatible) {}

void DocumentSourceGroupBase::addAccumulator(AccumulationStatement accumulationStatement) {
    _accumulatedFields.push_back(accumulationStatement);
    _memoryTracker.set(accumulationStatement.fieldName, 0);
}

namespace {

intrusive_ptr<Expression> parseIdExpression(const intrusive_ptr<ExpressionContext>& expCtx,
                                            BSONElement groupField,
                                            const VariablesParseState& vps) {
    if (groupField.type() == Object) {
        // {_id: {}} is treated as grouping on a constant, not an expression
        if (groupField.Obj().isEmpty()) {
            return ExpressionConstant::create(expCtx.get(), Value(groupField));
        }

        const BSONObj idKeyObj = groupField.Obj();
        if (idKeyObj.firstElementFieldName()[0] == '$') {
            // grouping on a $op expression
            return Expression::parseObject(expCtx.get(), idKeyObj, vps);
        } else {
            for (auto&& field : idKeyObj) {
                uassert(17390,
                        "$group does not support inclusion-style expressions",
                        !field.isNumber() && field.type() != Bool);
            }
            return ExpressionObject::parse(expCtx.get(), idKeyObj, vps);
        }
    } else {
        return Expression::parseOperand(expCtx.get(), groupField, vps);
    }
}

}  // namespace

void DocumentSourceGroupBase::setIdExpression(const boost::intrusive_ptr<Expression> idExpression) {
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

boost::intrusive_ptr<Expression> DocumentSourceGroupBase::getIdExpression() const {
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

void DocumentSourceGroupBase::initializeFromBson(BSONElement elem) {
    uassert(15947, "a group's fields must be specified in an object", elem.type() == Object);

    BSONObj groupObj(elem.Obj());
    BSONObjIterator groupIterator(groupObj);
    VariablesParseState vps = pExpCtx->variablesParseState;
    pExpCtx->sbeGroupCompatibility = SbeCompatibility::fullyCompatible;
    while (groupIterator.more()) {
        BSONElement groupField(groupIterator.next());
        StringData pFieldName = groupField.fieldNameStringData();
        if (pFieldName == "_id") {
            uassert(15948, "a group's _id may only be specified once", _idExpressions.empty());
            setIdExpression(parseIdExpression(pExpCtx, groupField, vps));
            invariant(!_idExpressions.empty());
        } else if (pFieldName == "$doingMerge") {
            massert(17030, "$doingMerge should be true if present", groupField.Bool());

            setDoingMerge(true);
        } else if (isSpecFieldReserved(pFieldName)) {
            // No-op: field is used by the derived class.
        } else {
            // Any other field will be treated as an accumulator specification.
            addAccumulator(
                AccumulationStatement::parseAccumulationStatement(pExpCtx.get(), groupField, vps));
        }
    }
    _sbeCompatibility = std::min(pExpCtx->sbeGroupCompatibility, pExpCtx->sbeCompatibility);

    uassert(15955, "a group specification must include an _id", !_idExpressions.empty());
}

namespace {

using GroupsMap = DocumentSourceGroupBase::GroupsMap;

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

void DocumentSourceGroupBase::processDocument(const Value& id, const Document& root) {
    const size_t numAccumulators = _accumulatedFields.size();

    // Look for the _id value in the map. If it's not there, add a new entry with a blank
    // accumulator. This is done in a somewhat odd way in order to avoid hashing 'id' and
    // looking it up in '_groups' multiple times.
    const size_t oldSize = _groups->size();
    vector<intrusive_ptr<AccumulatorState>>& group = (*_groups)[id];
    const bool inserted = _groups->size() != oldSize;

    vector<uint64_t> oldAccumMemUsage(numAccumulators, 0);
    if (inserted) {
        _memoryTracker.set(_memoryTracker.currentMemoryBytes() + id.getApproximateSize());

        // Initialize and add the accumulators
        Value expandedId = expandId(id);
        Document idDoc =
            expandedId.getType() == BSONType::Object ? expandedId.getDocument() : Document();
        group.reserve(numAccumulators);
        for (auto&& accumulatedField : _accumulatedFields) {
            auto accum = accumulatedField.makeAccumulator();
            Value initializerValue =
                accumulatedField.expr.initializer->evaluate(idDoc, &pExpCtx->variables);
            accum->startNewGroup(initializerValue);
            group.push_back(accum);
        }
    }

    /* tickle all the accumulators for the group we found */
    dassert(numAccumulators == group.size());

    for (size_t i = 0; i < numAccumulators; i++) {
        // Only process the input and update the memory footprint if the current accumulator
        // needs more input.
        if (group[i]->needsInput()) {
            const auto prevMemUsage = inserted ? 0 : group[i]->getMemUsage();
            group[i]->process(
                _accumulatedFields[i].expr.argument->evaluate(root, &pExpCtx->variables),
                _doingMerge);
            _memoryTracker.update(_accumulatedFields[i].fieldName,
                                  group[i]->getMemUsage() - prevMemUsage);
        }
    }

    if (kDebugBuild && !pExpCtx->opCtx->readOnly()) {
        // In debug mode, spill every time we have a duplicate id to stress merge logic.
        if (!inserted &&                      // is a dup
            !pExpCtx->inMongos &&             // can't spill to disk in mongos
            !_memoryTracker._allowDiskUse &&  // don't change behavior when testing external sort
            _sortedFiles.size() < 20) {       // don't open too many FDs
            spill();
        }
    }
}

void DocumentSourceGroupBase::readyGroups() {
    _spilled = !_sortedFiles.empty();
    if (_spilled) {
        if (!_groups->empty()) {
            spill();
        }

        _groups = pExpCtx->getValueComparator().makeUnorderedValueMap<Accumulators>();

        _sorterIterator.reset(Sorter<Value, Value>::Iterator::merge(
            _sortedFiles, SortOptions(), SorterComparator(pExpCtx->getValueComparator())));

        // prepare current to accumulate data
        _currentAccumulators.reserve(_accumulatedFields.size());
        for (auto&& accumulatedField : _accumulatedFields) {
            _currentAccumulators.push_back(accumulatedField.makeAccumulator());
        }

        verify(_sorterIterator->more());  // we put data in, we should get something out.
        _firstPartOfNextGroup = _sorterIterator->next();
    } else {
        // start the group iterator
        _groupsIterator = _groups->begin();
    }
}

void DocumentSourceGroupBase::resetReadyGroups() {
    // Free our resources.
    _groups = pExpCtx->getValueComparator().makeUnorderedValueMap<Accumulators>();
    _memoryTracker.resetCurrent();
    _sorterIterator.reset();
    _sortedFiles.clear();

    // Make us look done.
    _groupsIterator = _groups->end();
}

void DocumentSourceGroupBase::spill() {
    _stats.spills++;
    _stats.numBytesSpilledEstimate += _memoryTracker.currentMemoryBytes();
    _stats.spilledRecords += _groups->size();

    vector<const GroupsMap::value_type*> ptrs;  // using pointers to speed sorting
    ptrs.reserve(_groups->size());
    for (GroupsMap::const_iterator it = _groups->begin(), end = _groups->end(); it != end; ++it) {
        ptrs.push_back(&*it);
    }

    stable_sort(ptrs.begin(), ptrs.end(), SpillSTLComparator(pExpCtx->getValueComparator()));

    // Initialize '_file' in a lazy manner only when it is needed.
    if (!_file) {
        _spillStats = std::make_unique<SorterFileStats>(nullptr /* sorterTracker */);
        _file = std::make_shared<Sorter<Value, Value>::File>(
            pExpCtx->tempDir + "/" + nextFileName(), _spillStats.get());
    }
    SortedFileWriter<Value, Value> writer(SortOptions().TempDir(pExpCtx->tempDir), _file);
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
                vector<Value> accums;
                for (size_t j = 0; j < ptrs[i]->second.size(); j++) {
                    accums.push_back(ptrs[i]->second[j]->getValue(/*toBeMerged=*/true));
                }
                writer.addAlreadySorted(ptrs[i]->first, Value(std::move(accums)));
            }
            break;
    }

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(pExpCtx->opCtx);
    metricsCollector.incrementKeysSorted(ptrs.size());
    metricsCollector.incrementSorterSpills(1);

    _groups->clear();
    // Zero out the current per-accumulation statement memory consumption, as the memory has been
    // freed by spilling.
    _memoryTracker.resetCurrent();

    _sortedFiles.emplace_back(writer.done());
    if (_spillStats) {
        _stats.spilledDataStorageSize = _spillStats->bytesSpilled();
    }
}

Value DocumentSourceGroupBase::computeId(const Document& root) {
    // If only one expression, return result directly
    if (_idExpressions.size() == 1) {
        Value retValue = _idExpressions[0]->evaluate(root, &pExpCtx->variables);
        return retValue.missing() ? Value(BSONNULL) : std::move(retValue);
    }

    // Multiple expressions get results wrapped in a vector
    vector<Value> vals;
    vals.reserve(_idExpressions.size());
    for (size_t i = 0; i < _idExpressions.size(); i++) {
        vals.push_back(_idExpressions[i]->evaluate(root, &pExpCtx->variables));
    }
    return Value(std::move(vals));
}

Value DocumentSourceGroupBase::expandId(const Value& val) {
    // _id doesn't get wrapped in a document
    if (_idFieldNames.empty())
        return val;

    // _id is a single-field document containing val
    if (_idFieldNames.size() == 1)
        return Value(DOC(_idFieldNames[0] << val));

    // _id is a multi-field document containing the elements of val
    const vector<Value>& vals = val.getArray();
    invariant(_idFieldNames.size() == vals.size());
    MutableDocument md(vals.size());
    for (size_t i = 0; i < vals.size(); i++) {
        md[_idFieldNames[i]] = vals[i];
    }
    return md.freezeToValue();
}

Document DocumentSourceGroupBase::makeDocument(const Value& id,
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

bool DocumentSourceGroupBase::pathIncludedInGroupKeys(const std::string& dottedPath) const {
    return std::any_of(
        _idExpressions.begin(), _idExpressions.end(), [&dottedPath](const auto& exp) {
            if (auto fieldExp = dynamic_cast<ExpressionFieldPath*>(exp.get())) {
                if (fieldExp->representsPath(dottedPath)) {
                    return true;
                }
            }
            return false;
        });
}

bool DocumentSourceGroupBase::canRunInParallelBeforeWriteStage(
    const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) const {
    if (_doingMerge) {
        return true;  // This is fine.
    }

    // Certain $group stages are allowed to execute on each exchange consumer. In order to
    // guarantee each consumer will only group together data from its own shard, the $group must
    // group on a superset of the shard key.
    for (auto&& currentPathOfShardKey : nameOfShardKeyFieldsUponEntryToStage) {
        if (!pathIncludedInGroupKeys(currentPathOfShardKey)) {
            // This requires an exact path match, but as a future optimization certain path
            // prefixes should be okay. For example, if the shard key path is "a.b", and we're
            // grouping by "a", then each group of "a" is strictly more specific than "a.b", so
            // we can deduce that grouping by "a" will not need to group together documents
            // across different values of the shard key field "a.b", and thus as long as any
            // other shard key fields are similarly preserved will not need to consume a merged
            // stream to perform the group.
            return false;
        }
    }
    return true;
}

std::unique_ptr<GroupFromFirstDocumentTransformation>
DocumentSourceGroupBase::rewriteGroupAsTransformOnFirstDocument() const {
    if (_idExpressions.size() != 1) {
        // This transformation is only intended for $group stages that group on a single field.
        return nullptr;
    }

    auto fieldPathExpr = dynamic_cast<ExpressionFieldPath*>(_idExpressions.front().get());
    if (!fieldPathExpr || fieldPathExpr->isVariableReference()) {
        return nullptr;
    }

    const auto fieldPath = fieldPathExpr->getFieldPath();
    if (fieldPath.getPathLength() == 1) {
        // The path is $$CURRENT or $$ROOT. This isn't really a sensible value to group by (since
        // each document has a unique _id, it will just return the entire collection). We only
        // apply the rewrite when grouping by a single field, so we cannot apply it in this case,
        // where we are grouping by the entire document.
        tassert(5943200,
                "Optimization attempted on group by always-dissimilar system variable",
                fieldPath.getFieldName(0) == "CURRENT" || fieldPath.getFieldName(0) == "ROOT");
        return nullptr;
    }

    const auto groupId = fieldPath.tail().fullPath();

    // We do this transformation only if there are all $first, all $last, or no accumulators.
    GroupFromFirstDocumentTransformation::ExpectedInput expectedInput;
    if (accsNeedSameDoc(_accumulatedFields, AccumulatorDocumentsNeeded::kFirstDocument)) {
        expectedInput = GroupFromFirstDocumentTransformation::ExpectedInput::kFirstDocument;
    } else if (accsNeedSameDoc(_accumulatedFields, AccumulatorDocumentsNeeded::kLastDocument)) {
        expectedInput = GroupFromFirstDocumentTransformation::ExpectedInput::kLastDocument;
    } else {
        return nullptr;
    }

    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> fields;

    boost::intrusive_ptr<Expression> idField;
    // The _id field can be specified either as a fieldpath (ex. _id: "$a") or as a singleton
    // object (ex. _id: {v: "$a"}).
    if (_idFieldNames.empty()) {
        idField = ExpressionFieldPath::deprecatedCreate(pExpCtx.get(), groupId);
    } else {
        invariant(_idFieldNames.size() == 1);
        idField = ExpressionObject::create(pExpCtx.get(),
                                           {{_idFieldNames.front(), _idExpressions.front()}});
    }
    fields.emplace_back("_id", idField);

    for (auto&& accumulator : _accumulatedFields) {
        fields.emplace_back(accumulator.fieldName, accumulator.expr.argument);

        // Since we don't attempt this transformation for non-$first/$last accumulators,
        // the initializer should always be trivial.
    }

    return GroupFromFirstDocumentTransformation::create(
        pExpCtx, groupId, getSourceName(), std::move(fields), expectedInput);
}

size_t DocumentSourceGroupBase::getMaxMemoryUsageBytes() const {
    return _memoryTracker._maxAllowedMemoryUsageBytes;
}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceGroupBase::distributedPlanLogic() {
    VariablesParseState vps = pExpCtx->variablesParseState;
    /* the merger will use the same grouping key */
    auto mergerGroupByExpression = ExpressionFieldPath::parse(pExpCtx.get(), "$$ROOT._id", vps);

    std::vector<AccumulationStatement> mergerAccumulators;
    mergerAccumulators.reserve(_accumulatedFields.size());
    for (auto&& accumulatedField : _accumulatedFields) {
        // The merger's output field names will be the same, as will the accumulator factories.
        // However, for some accumulators, the expression to be accumulated will be different. The
        // original accumulator may be collecting an expression based on a field expression or
        // constant.  Here, we accumulate the output of the same name from the prior group.
        auto copiedAccumulatedField = accumulatedField;
        copiedAccumulatedField.expr.argument = ExpressionFieldPath::parse(
            pExpCtx.get(), "$$ROOT." + copiedAccumulatedField.fieldName, vps);
        mergerAccumulators.emplace_back(std::move(copiedAccumulatedField));
    }

    // When merging, we always use generic hash based algorithm.
    boost::intrusive_ptr<DocumentSourceGroup> mergingGroup = DocumentSourceGroup::create(
        pExpCtx, std::move(mergerGroupByExpression), std::move(mergerAccumulators));
    mergingGroup->setDoingMerge(true);
    // {shardsStage, mergingStage, sortPattern}
    return DistributedPlanLogic{this, mergingGroup, boost::none};
}

}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
// Explicit instantiation unneeded since we aren't exposing Sorter outside of this file.
