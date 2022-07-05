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
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
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

}  // namespace

using boost::intrusive_ptr;
using std::pair;
using std::shared_ptr;
using std::vector;

Document GroupFromFirstDocumentTransformation::applyTransformation(const Document& input) {
    MutableDocument output(_accumulatorExprs.size());

    for (auto&& expr : _accumulatorExprs) {
        auto value = expr.second->evaluate(input, &expr.second->getExpressionContext()->variables);
        output.addField(expr.first, value.missing() ? Value(BSONNULL) : std::move(value));
    }

    return output.freeze();
}

void GroupFromFirstDocumentTransformation::optimize() {
    for (auto&& expr : _accumulatorExprs) {
        expr.second = expr.second->optimize();
    }
}

Document GroupFromFirstDocumentTransformation::serializeTransformation(
    boost::optional<ExplainOptions::Verbosity> explain) const {

    MutableDocument newRoot(_accumulatorExprs.size());
    for (auto&& expr : _accumulatorExprs) {
        newRoot.addField(expr.first, expr.second->serialize(static_cast<bool>(explain)));
    }

    return {{"newRoot", newRoot.freezeToValue()}};
}

DepsTracker::State GroupFromFirstDocumentTransformation::addDependencies(DepsTracker* deps) const {
    for (auto&& expr : _accumulatorExprs) {
        expr.second->addDependencies(deps);
    }

    // This stage will replace the entire document with a new document, so any existing fields
    // will be replaced and cannot be required as dependencies. We use EXHAUSTIVE_ALL here
    // instead of EXHAUSTIVE_FIELDS, as in ReplaceRootTransformation, because the stages that
    // follow a $group stage should not depend on document metadata.
    return DepsTracker::State::EXHAUSTIVE_ALL;
}

DocumentSource::GetModPathsReturn GroupFromFirstDocumentTransformation::getModifiedPaths() const {
    // Replaces the entire root, so all paths are modified.
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
}

std::unique_ptr<GroupFromFirstDocumentTransformation> GroupFromFirstDocumentTransformation::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    const std::string& groupId,
    vector<pair<std::string, intrusive_ptr<Expression>>> accumulatorExprs) {
    return std::make_unique<GroupFromFirstDocumentTransformation>(groupId,
                                                                  std::move(accumulatorExprs));
}

constexpr StringData DocumentSourceGroup::kStageName;

REGISTER_DOCUMENT_SOURCE(group,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceGroup::createFromBson,
                         AllowedWithApiStrict::kAlways);

const char* DocumentSourceGroup::getSourceName() const {
    return kStageName.rawData();
}

bool DocumentSourceGroup::shouldSpillWithAttemptToSaveMemory() {
    if (!_memoryTracker._allowDiskUse &&
        (_memoryTracker.currentMemoryBytes() >
         static_cast<long long>(_memoryTracker._maxAllowedMemoryUsageBytes))) {
        freeMemory();
    }

    if (_memoryTracker.currentMemoryBytes() >
        static_cast<long long>(_memoryTracker._maxAllowedMemoryUsageBytes)) {
        uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
                "Exceeded memory limit for $group, but didn't allow external sort."
                " Pass allowDiskUse:true to opt in.",
                _memoryTracker._allowDiskUse);
        _memoryTracker.resetCurrent();
        return true;
    }
    return false;
}

void DocumentSourceGroup::freeMemory() {
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

DocumentSource::GetNextResult DocumentSourceGroup::doGetNext() {
    if (!_initialized) {
        const auto initializationResult = initialize();
        if (initializationResult.isPaused()) {
            return initializationResult;
        }
        invariant(initializationResult.isEOF());
    }

    for (auto&& accum : _currentAccumulators) {
        accum->reset();  // Prep accumulators for a new group.
    }

    if (_spilled) {
        return getNextSpilled();
    } else {
        return getNextStandard();
    }
}

DocumentSource::GetNextResult DocumentSourceGroup::getNextSpilled() {
    // We aren't streaming, and we have spilled to disk.
    if (!_sorterIterator)
        return GetNextResult::makeEOF();

    _currentId = _firstPartOfNextGroup.first;
    const size_t numAccumulators = _accumulatedFields.size();

    // Call startNewGroup on every accumulator.
    Value expandedId = expandId(_currentId);
    Document idDoc =
        expandedId.getType() == BSONType::Object ? expandedId.getDocument() : Document();
    for (size_t i = 0; i < numAccumulators; ++i) {
        Value initializerValue =
            _accumulatedFields[i].expr.initializer->evaluate(idDoc, &pExpCtx->variables);
        _currentAccumulators[i]->startNewGroup(initializerValue);
    }

    while (pExpCtx->getValueComparator().evaluate(_currentId == _firstPartOfNextGroup.first)) {
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
            dispose();
            break;
        }

        _firstPartOfNextGroup = _sorterIterator->next();
    }

    return makeDocument(_currentId, _currentAccumulators, pExpCtx->needsMerge);
}

DocumentSource::GetNextResult DocumentSourceGroup::getNextStandard() {
    // Not spilled, and not streaming.
    if (_groups->empty())
        return GetNextResult::makeEOF();

    Document out = makeDocument(groupsIterator->first, groupsIterator->second, pExpCtx->needsMerge);

    if (++groupsIterator == _groups->end())
        dispose();

    return out;
}

void DocumentSourceGroup::doDispose() {
    // Free our resources.
    _groups = pExpCtx->getValueComparator().makeUnorderedValueMap<Accumulators>();
    _sorterIterator.reset();

    // Make us look done.
    groupsIterator = _groups->end();
}

intrusive_ptr<DocumentSource> DocumentSourceGroup::optimize() {
    // Optimizing a 'DocumentSourceGroup' might modify its expressions to become incompatible with
    // SBE. We temporarily highjack the context's 'sbeCompatible' flag to communicate the situation
    // back to the 'DocumentSourceGroup'. Notice, that while a particular 'DocumentSourceGroup'
    // might become incompatible with SBE, other groups in the pipeline and the collection access
    // could be still eligible for lowering to SBE, thus we must reset the context's 'sbeCompatible'
    // flag back to its original value at the end of the 'optimize()' call.
    //
    // TODO SERVER-XXXXX: replace this hack with a proper per-stage tracking of SBE compatibility.
    auto expCtx = _idExpressions[0]->getExpressionContext();
    auto orgSbeCompatible = expCtx->sbeCompatible;
    expCtx->sbeCompatible = true;

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

    _sbeCompatible = _sbeCompatible && expCtx->sbeCompatible;
    expCtx->sbeCompatible = orgSbeCompatible;

    return this;
}

Value DocumentSourceGroup::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    MutableDocument insides;

    // Add the _id.
    if (_idFieldNames.empty()) {
        invariant(_idExpressions.size() == 1);
        insides["_id"] = _idExpressions[0]->serialize(static_cast<bool>(explain));
    } else {
        // Decomposed document case.
        invariant(_idExpressions.size() == _idFieldNames.size());
        MutableDocument md;
        for (size_t i = 0; i < _idExpressions.size(); i++) {
            md[_idFieldNames[i]] = _idExpressions[i]->serialize(static_cast<bool>(explain));
        }
        insides["_id"] = md.freezeToValue();
    }

    // Add the remaining fields.
    for (auto&& accumulatedField : _accumulatedFields) {
        intrusive_ptr<AccumulatorState> accum = accumulatedField.makeAccumulator();
        insides[accumulatedField.fieldName] =
            Value(accum->serialize(accumulatedField.expr.initializer,
                                   accumulatedField.expr.argument,
                                   static_cast<bool>(explain)));
    }

    if (_doingMerge) {
        // This makes the output unparsable (with error) on pre 2.6 shards, but it will never
        // be sent to old shards when this flag is true since they can't do a merge anyway.
        insides["$doingMerge"] = Value(true);
    }

    MutableDocument out;
    out[getSourceName()] = Value(insides.freeze());

    if (explain && *explain >= ExplainOptions::Verbosity::kExecStats) {
        MutableDocument md;

        for (size_t i = 0; i < _accumulatedFields.size(); i++) {
            md[_accumulatedFields[i].fieldName] = Value(static_cast<long long>(
                _memoryTracker[_accumulatedFields[i].fieldName].maxMemoryBytes()));
        }

        out["maxAccumulatorMemoryUsageBytes"] = Value(md.freezeToValue());
        out["totalOutputDataSizeBytes"] =
            Value(static_cast<long long>(_stats.totalOutputDataSizeBytes));
        out["usedDisk"] = Value(_stats.spills > 0);
        out["spills"] = Value(static_cast<long long>(_stats.spills));
    }

    return Value(out.freezeToValue());
}

DepsTracker::State DocumentSourceGroup::getDependencies(DepsTracker* deps) const {
    // add the _id
    for (size_t i = 0; i < _idExpressions.size(); i++) {
        _idExpressions[i]->addDependencies(deps);
    }

    // add the rest
    for (auto&& accumulatedField : _accumulatedFields) {
        accumulatedField.expr.argument->addDependencies(deps);
        // Don't add initializer, because it doesn't refer to docs from the input stream.
    }

    return DepsTracker::State::EXHAUSTIVE_ALL;
}

DocumentSource::GetModPathsReturn DocumentSourceGroup::getModifiedPaths() const {
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

StringMap<boost::intrusive_ptr<Expression>> DocumentSourceGroup::getIdFields() const {
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

const std::vector<AccumulationStatement>& DocumentSourceGroup::getAccumulatedFields() const {
    return _accumulatedFields;
}

intrusive_ptr<DocumentSourceGroup> DocumentSourceGroup::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<Expression>& groupByExpression,
    std::vector<AccumulationStatement> accumulationStatements,
    boost::optional<size_t> maxMemoryUsageBytes) {
    intrusive_ptr<DocumentSourceGroup> groupStage(
        new DocumentSourceGroup(expCtx, maxMemoryUsageBytes));
    groupStage->setIdExpression(groupByExpression);
    for (auto&& statement : accumulationStatements) {
        groupStage->addAccumulator(statement);
        groupStage->_memoryTracker.set(statement.fieldName, 0);
    }

    return groupStage;
}

DocumentSourceGroup::DocumentSourceGroup(const intrusive_ptr<ExpressionContext>& expCtx,
                                         boost::optional<size_t> maxMemoryUsageBytes)
    : DocumentSource(kStageName, expCtx),
      _doingMerge(false),
      _memoryTracker{expCtx->allowDiskUse && !expCtx->inMongos,
                     maxMemoryUsageBytes
                         ? *maxMemoryUsageBytes
                         : static_cast<size_t>(internalDocumentSourceGroupMaxMemoryBytes.load())},
      _initialized(false),
      _groups(expCtx->getValueComparator().makeUnorderedValueMap<Accumulators>()),
      _spilled(false),
      _sbeCompatible(false) {}

void DocumentSourceGroup::addAccumulator(AccumulationStatement accumulationStatement) {
    _accumulatedFields.push_back(accumulationStatement);
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

void DocumentSourceGroup::setIdExpression(const boost::intrusive_ptr<Expression> idExpression) {
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

boost::intrusive_ptr<Expression> DocumentSourceGroup::getIdExpression() const {
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

intrusive_ptr<DocumentSource> DocumentSourceGroup::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(15947, "a group's fields must be specified in an object", elem.type() == Object);

    intrusive_ptr<DocumentSourceGroup> groupStage(new DocumentSourceGroup(expCtx));

    BSONObj groupObj(elem.Obj());
    BSONObjIterator groupIterator(groupObj);
    VariablesParseState vps = expCtx->variablesParseState;
    expCtx->sbeGroupCompatible = true;
    while (groupIterator.more()) {
        BSONElement groupField(groupIterator.next());
        StringData pFieldName = groupField.fieldNameStringData();
        if (pFieldName == "_id") {
            uassert(15948,
                    "a group's _id may only be specified once",
                    groupStage->_idExpressions.empty());
            groupStage->setIdExpression(parseIdExpression(expCtx, groupField, vps));
            invariant(!groupStage->_idExpressions.empty());
        } else if (pFieldName == "$doingMerge") {
            massert(17030, "$doingMerge should be true if present", groupField.Bool());

            groupStage->setDoingMerge(true);
        } else {
            // Any other field will be treated as an accumulator specification.
            groupStage->addAccumulator(
                AccumulationStatement::parseAccumulationStatement(expCtx.get(), groupField, vps));
            groupStage->_memoryTracker.set(pFieldName, 0);
        }
    }
    groupStage->_sbeCompatible = expCtx->sbeGroupCompatible && expCtx->sbeCompatible;

    uassert(
        15955, "a group specification must include an _id", !groupStage->_idExpressions.empty());
    return groupStage;
}

namespace {

using GroupsMap = DocumentSourceGroup::GroupsMap;

class SorterComparator {
public:
    typedef pair<Value, Value> Data;

    SorterComparator(ValueComparator valueComparator) : _valueComparator(valueComparator) {}

    int operator()(const Data& lhs, const Data& rhs) const {
        return _valueComparator.compare(lhs.first, rhs.first);
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

DocumentSource::GetNextResult DocumentSourceGroup::initialize() {
    GetNextResult input = pSource->getNext();
    return initializeSelf(input);
}

// This separate NOINLINE function is used here to decrease stack utilization of initialize() and
// prevent stack overflows.
MONGO_COMPILER_NOINLINE DocumentSource::GetNextResult DocumentSourceGroup::initializeSelf(
    GetNextResult input) {
    const size_t numAccumulators = _accumulatedFields.size();
    // Barring any pausing, this loop exhausts 'pSource' and populates '_groups'.
    for (; input.isAdvanced(); input = pSource->getNext()) {
        if (shouldSpillWithAttemptToSaveMemory()) {
            _sortedFiles.push_back(spill());
        }

        // We release the result document here so that it does not outlive the end of this loop
        // iteration. Not releasing could lead to an array copy when this group follows an unwind.
        auto rootDocument = input.releaseDocument();
        Value id = computeId(rootDocument);

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
                group[i]->process(_accumulatedFields[i].expr.argument->evaluate(
                                      rootDocument, &pExpCtx->variables),
                                  _doingMerge);
                _memoryTracker.update(_accumulatedFields[i].fieldName,
                                      group[i]->getMemUsage() - prevMemUsage);
            }
        }

        if (kDebugBuild && !pExpCtx->opCtx->readOnly()) {
            // In debug mode, spill every time we have a duplicate id to stress merge logic.
            if (!inserted &&           // is a dup
                !pExpCtx->inMongos &&  // can't spill to disk in mongos
                !_memoryTracker
                     ._allowDiskUse &&       // don't change behavior when testing external sort
                _sortedFiles.size() < 20) {  // don't open too many FDs

                _sortedFiles.push_back(spill());
            }
        }
    }

    switch (input.getStatus()) {
        case DocumentSource::GetNextResult::ReturnStatus::kAdvanced: {
            MONGO_UNREACHABLE;  // We consumed all advances above.
        }
        case DocumentSource::GetNextResult::ReturnStatus::kPauseExecution: {
            return input;  // Propagate pause.
        }
        case DocumentSource::GetNextResult::ReturnStatus::kEOF: {
            // Do any final steps necessary to prepare to output results.
            if (!_sortedFiles.empty()) {
                _spilled = true;
                if (!_groups->empty()) {
                    _sortedFiles.push_back(spill());
                }

                // We won't be using groups again so free its memory.
                _groups = pExpCtx->getValueComparator().makeUnorderedValueMap<Accumulators>();

                _sorterIterator.reset(Sorter<Value, Value>::Iterator::merge(
                    _sortedFiles, SortOptions(), SorterComparator(pExpCtx->getValueComparator())));

                // prepare current to accumulate data
                _currentAccumulators.reserve(numAccumulators);
                for (auto&& accumulatedField : _accumulatedFields) {
                    _currentAccumulators.push_back(accumulatedField.makeAccumulator());
                }

                verify(_sorterIterator->more());  // we put data in, we should get something out.
                _firstPartOfNextGroup = _sorterIterator->next();
            } else {
                // start the group iterator
                groupsIterator = _groups->begin();
            }

            // This must happen last so that, unless control gets here, we will re-enter
            // initialization after getting a GetNextResult::ResultState::kPauseExecution.
            _initialized = true;
            return input;
        }
    }
    MONGO_UNREACHABLE;
}

shared_ptr<Sorter<Value, Value>::Iterator> DocumentSourceGroup::spill() {
    _stats.spills++;

    vector<const GroupsMap::value_type*> ptrs;  // using pointers to speed sorting
    ptrs.reserve(_groups->size());
    for (GroupsMap::const_iterator it = _groups->begin(), end = _groups->end(); it != end; ++it) {
        ptrs.push_back(&*it);
    }

    stable_sort(ptrs.begin(), ptrs.end(), SpillSTLComparator(pExpCtx->getValueComparator()));

    // Initialize '_file' in a lazy manner only when it is needed.
    if (!_file) {
        _file =
            std::make_shared<Sorter<Value, Value>::File>(pExpCtx->tempDir + "/" + nextFileName());
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
    for (auto accum : _accumulatedFields) {
        _memoryTracker.set(accum.fieldName, 0);
    }

    Sorter<Value, Value>::Iterator* iteratorPtr = writer.done();
    return shared_ptr<Sorter<Value, Value>::Iterator>(iteratorPtr);
}

Value DocumentSourceGroup::computeId(const Document& root) {
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

Value DocumentSourceGroup::expandId(const Value& val) {
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

Document DocumentSourceGroup::makeDocument(const Value& id,
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

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceGroup::distributedPlanLogic() {
    intrusive_ptr<DocumentSourceGroup> mergingGroup(new DocumentSourceGroup(pExpCtx));
    mergingGroup->setDoingMerge(true);

    VariablesParseState vps = pExpCtx->variablesParseState;
    /* the merger will use the same grouping key */
    mergingGroup->setIdExpression(ExpressionFieldPath::parse(pExpCtx.get(), "$$ROOT._id", vps));

    for (auto&& accumulatedField : _accumulatedFields) {
        // The merger's output field names will be the same, as will the accumulator factories.
        // However, for some accumulators, the expression to be accumulated will be different. The
        // original accumulator may be collecting an expression based on a field expression or
        // constant.  Here, we accumulate the output of the same name from the prior group.
        auto copiedAccumulatedField = accumulatedField;
        copiedAccumulatedField.expr.argument = ExpressionFieldPath::parse(
            pExpCtx.get(), "$$ROOT." + copiedAccumulatedField.fieldName, vps);
        mergingGroup->addAccumulator(copiedAccumulatedField);
        mergingGroup->_memoryTracker.set(copiedAccumulatedField.fieldName, 0);
    }

    // {shardsStage, mergingStage, sortPattern}
    return DistributedPlanLogic{this, mergingGroup, boost::none};
}

bool DocumentSourceGroup::pathIncludedInGroupKeys(const std::string& dottedPath) const {
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

bool DocumentSourceGroup::canRunInParallelBeforeWriteStage(
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
DocumentSourceGroup::rewriteGroupAsTransformOnFirstDocument() const {
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

    // We can't do this transformation if there are any non-$first accumulators.
    for (auto&& accumulator : _accumulatedFields) {
        if (AccumulatorDocumentsNeeded::kFirstDocument !=
            accumulator.makeAccumulator()->documentsNeeded()) {
            return nullptr;
        }
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
    fields.push_back(std::make_pair("_id", idField));

    for (auto&& accumulator : _accumulatedFields) {
        fields.push_back(std::make_pair(accumulator.fieldName, accumulator.expr.argument));

        // Since we don't attempt this transformation for non-$first accumulators,
        // the initializer should always be trivial.
    }

    return GroupFromFirstDocumentTransformation::create(pExpCtx, groupId, std::move(fields));
}

size_t DocumentSourceGroup::getMaxMemoryUsageBytes() const {
    return _memoryTracker._maxAllowedMemoryUsageBytes;
}

}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
// Explicit instantiation unneeded since we aren't exposing Sorter outside of this file.
