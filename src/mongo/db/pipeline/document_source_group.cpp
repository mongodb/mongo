/**
*    Copyright (C) 2011 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::pair;
using std::vector;

REGISTER_DOCUMENT_SOURCE(group,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceGroup::createFromBson);

const char* DocumentSourceGroup::getSourceName() const {
    return "$group";
}

DocumentSource::GetNextResult DocumentSourceGroup::getNext() {
    pExpCtx->checkForInterrupt();

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
    } else if (_streaming) {
        return getNextStreaming();
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
    while (pExpCtx->getValueComparator().evaluate(_currentId == _firstPartOfNextGroup.first)) {
        // Inside of this loop, _firstPartOfNextGroup is the current data being processed.
        // At loop exit, it is the first value to be processed in the next group.
        switch (numAccumulators) {  // mirrors switch in spill()
            case 1:                 // Single accumulators serialize as a single Value.
                _currentAccumulators[0]->process(_firstPartOfNextGroup.second, true);
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

    return std::move(out);
}

DocumentSource::GetNextResult DocumentSourceGroup::getNextStreaming() {
    // Streaming optimization is active.
    if (!_firstDocOfNextGroup) {
        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }
        _firstDocOfNextGroup = nextInput.releaseDocument();
    }

    Value id;
    do {
        // Add to the current accumulator(s).
        for (size_t i = 0; i < _currentAccumulators.size(); i++) {
            _currentAccumulators[i]->process(
                _accumulatedFields[i].expression->evaluate(*_firstDocOfNextGroup), _doingMerge);
        }

        // Retrieve the next document.
        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }

        _firstDocOfNextGroup = nextInput.releaseDocument();


        // Compute the id. If it does not match _currentId, we will exit the loop, leaving
        // _firstDocOfNextGroup set for the next time getNext() is called.
        id = computeId(*_firstDocOfNextGroup);
    } while (pExpCtx->getValueComparator().evaluate(_currentId == id));

    Document out = makeDocument(_currentId, _currentAccumulators, pExpCtx->needsMerge);
    _currentId = std::move(id);

    return std::move(out);
}

void DocumentSourceGroup::doDispose() {
    // Free our resources.
    _groups = pExpCtx->getValueComparator().makeUnorderedValueMap<Accumulators>();
    _sorterIterator.reset();

    // Make us look done.
    groupsIterator = _groups->end();

    _firstDocOfNextGroup = boost::none;
}

intrusive_ptr<DocumentSource> DocumentSourceGroup::optimize() {
    // TODO: If all _idExpressions are ExpressionConstants after optimization, then we know there
    // will be only one group. We should take advantage of that to avoid going through the hash
    // table.
    for (size_t i = 0; i < _idExpressions.size(); i++) {
        _idExpressions[i] = _idExpressions[i]->optimize();
    }

    for (auto&& accumulatedField : _accumulatedFields) {
        accumulatedField.expression = accumulatedField.expression->optimize();
    }

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
        intrusive_ptr<Accumulator> accum = accumulatedField.makeAccumulator(pExpCtx);
        insides[accumulatedField.fieldName] =
            Value(DOC(accum->getOpName()
                      << accumulatedField.expression->serialize(static_cast<bool>(explain))));
    }

    if (_doingMerge) {
        // This makes the output unparsable (with error) on pre 2.6 shards, but it will never
        // be sent to old shards when this flag is true since they can't do a merge anyway.
        insides["$doingMerge"] = Value(true);
    }

    if (explain && findRelevantInputSort()) {
        return Value(DOC("$streamingGroup" << insides.freeze()));
    }
    return Value(DOC(getSourceName() << insides.freeze()));
}

DepsTracker::State DocumentSourceGroup::getDependencies(DepsTracker* deps) const {
    // add the _id
    for (size_t i = 0; i < _idExpressions.size(); i++) {
        _idExpressions[i]->addDependencies(deps);
    }

    // add the rest
    for (auto&& accumulatedField : _accumulatedFields) {
        accumulatedField.expression->addDependencies(deps);
    }

    return DepsTracker::State::EXHAUSTIVE_ALL;
}

intrusive_ptr<DocumentSourceGroup> DocumentSourceGroup::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    const boost::intrusive_ptr<Expression>& groupByExpression,
    std::vector<AccumulationStatement> accumulationStatements,
    size_t maxMemoryUsageBytes) {
    intrusive_ptr<DocumentSourceGroup> groupStage(
        new DocumentSourceGroup(pExpCtx, maxMemoryUsageBytes));
    groupStage->setIdExpression(groupByExpression);
    for (auto&& statement : accumulationStatements) {
        groupStage->addAccumulator(statement);
    }

    return groupStage;
}

DocumentSourceGroup::DocumentSourceGroup(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                         size_t maxMemoryUsageBytes)
    : DocumentSource(pExpCtx),
      _doingMerge(false),
      _maxMemoryUsageBytes(maxMemoryUsageBytes),
      _inputSort(BSONObj()),
      _streaming(false),
      _initialized(false),
      _groups(pExpCtx->getValueComparator().makeUnorderedValueMap<Accumulators>()),
      _spilled(false),
      _allowDiskUse(pExpCtx->allowDiskUse && !pExpCtx->inMongos) {}

void DocumentSourceGroup::addAccumulator(AccumulationStatement accumulationStatement) {
    _accumulatedFields.push_back(accumulationStatement);
}

namespace {

intrusive_ptr<Expression> parseIdExpression(const intrusive_ptr<ExpressionContext>& expCtx,
                                            BSONElement groupField,
                                            const VariablesParseState& vps) {
    if (groupField.type() == Object && !groupField.Obj().isEmpty()) {
        // {_id: {}} is treated as grouping on a constant, not an expression

        const BSONObj idKeyObj = groupField.Obj();
        if (idKeyObj.firstElementFieldName()[0] == '$') {
            // grouping on a $op expression
            return Expression::parseObject(expCtx, idKeyObj, vps);
        } else {
            for (auto&& field : idKeyObj) {
                uassert(17390,
                        "$group does not support inclusion-style expressions",
                        !field.isNumber() && field.type() != Bool);
            }
            return ExpressionObject::parse(expCtx, idKeyObj, vps);
        }
    } else if (groupField.type() == String && groupField.valuestr()[0] == '$') {
        // grouping on a field path.
        return ExpressionFieldPath::parse(expCtx, groupField.str(), vps);
    } else {
        // constant id - single group
        return ExpressionConstant::create(expCtx, Value(groupField));
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

intrusive_ptr<DocumentSource> DocumentSourceGroup::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(15947, "a group's fields must be specified in an object", elem.type() == Object);

    intrusive_ptr<DocumentSourceGroup> pGroup(new DocumentSourceGroup(pExpCtx));

    BSONObj groupObj(elem.Obj());
    BSONObjIterator groupIterator(groupObj);
    VariablesParseState vps = pExpCtx->variablesParseState;
    while (groupIterator.more()) {
        BSONElement groupField(groupIterator.next());
        const char* pFieldName = groupField.fieldName();

        if (str::equals(pFieldName, "_id")) {
            uassert(
                15948, "a group's _id may only be specified once", pGroup->_idExpressions.empty());
            pGroup->setIdExpression(parseIdExpression(pExpCtx, groupField, vps));
            invariant(!pGroup->_idExpressions.empty());
        } else if (str::equals(pFieldName, "$doingMerge")) {
            massert(17030, "$doingMerge should be true if present", groupField.Bool());

            pGroup->setDoingMerge(true);
        } else {
            // Any other field will be treated as an accumulator specification.
            pGroup->addAccumulator(
                AccumulationStatement::parseAccumulationStatement(pExpCtx, groupField, vps));
        }
    }

    uassert(15955, "a group specification must include an _id", !pGroup->_idExpressions.empty());
    return pGroup;
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

bool containsOnlyFieldPathsAndConstants(ExpressionObject* expressionObj) {
    for (auto&& it : expressionObj->getChildExpressions()) {
        const intrusive_ptr<Expression>& childExp = it.second;
        if (dynamic_cast<ExpressionFieldPath*>(childExp.get())) {
            continue;
        } else if (dynamic_cast<ExpressionConstant*>(childExp.get())) {
            continue;
        } else if (auto expObj = dynamic_cast<ExpressionObject*>(childExp.get())) {
            if (!containsOnlyFieldPathsAndConstants(expObj)) {
                // A nested expression was not a FieldPath or a constant.
                return false;
            }
        } else {
            // expressionObj was something other than a FieldPath, a constant, or a nested object.
            return false;
        }
    }
    return true;
}

void getFieldPathMap(ExpressionObject* expressionObj,
                     std::string prefix,
                     StringMap<std::string>* fields) {
    // Given an expression with only constant and FieldPath leaf nodes, such as {x: {y: "$a.b"}},
    // attempt to compute a map from each FieldPath leaf to the path of that leaf. In the example,
    // this method would return: {"a.b" : "x.y"}.

    for (auto&& it : expressionObj->getChildExpressions()) {
        intrusive_ptr<Expression> childExp = it.second;
        ExpressionObject* expObj = dynamic_cast<ExpressionObject*>(childExp.get());
        ExpressionFieldPath* expPath = dynamic_cast<ExpressionFieldPath*>(childExp.get());

        std::string newPrefix = prefix.empty() ? it.first : prefix + "." + it.first;

        if (expObj) {
            getFieldPathMap(expObj, newPrefix, fields);
        } else if (expPath) {
            (*fields)[expPath->getFieldPath().tail().fullPath()] = newPrefix;
        }
    }
}

void getFieldPathListForSpilled(ExpressionObject* expressionObj,
                                std::string prefix,
                                std::vector<std::string>* fields) {
    // Given an expression, attempt to compute a vector of strings, each representing the path
    // through the object to a leaf. For example, for the expression represented by
    // {x: 2, y: {z: "$a.b"}}, the output would be ["x", "y.z"].
    for (auto&& it : expressionObj->getChildExpressions()) {
        intrusive_ptr<Expression> childExp = it.second;
        ExpressionObject* expObj = dynamic_cast<ExpressionObject*>(childExp.get());

        std::string newPrefix = prefix.empty() ? it.first : prefix + "." + it.first;

        if (expObj) {
            getFieldPathListForSpilled(expObj, newPrefix, fields);
        } else {
            fields->push_back(newPrefix);
        }
    }
}
}  // namespace

DocumentSource::GetNextResult DocumentSourceGroup::initialize() {
    const size_t numAccumulators = _accumulatedFields.size();

    boost::optional<BSONObj> inputSort = findRelevantInputSort();
    if (inputSort) {
        // We can convert to streaming.
        _streaming = true;
        _inputSort = *inputSort;

        // Set up accumulators.
        _currentAccumulators.reserve(numAccumulators);
        for (auto&& accumulatedField : _accumulatedFields) {
            _currentAccumulators.push_back(accumulatedField.makeAccumulator(pExpCtx));
        }

        // We only need to load the first document.
        auto firstInput = pSource->getNext();
        if (!firstInput.isAdvanced()) {
            // Leave '_firstDocOfNextGroup' uninitialized and return.
            return firstInput;
        }
        _firstDocOfNextGroup = firstInput.releaseDocument();

        // Compute the _id value.
        _currentId = computeId(*_firstDocOfNextGroup);
        _initialized = true;
        return DocumentSource::GetNextResult::makeEOF();
    }


    // Barring any pausing, this loop exhausts 'pSource' and populates '_groups'.
    GetNextResult input = pSource->getNext();
    for (; input.isAdvanced(); input = pSource->getNext()) {
        if (_memoryUsageBytes > _maxMemoryUsageBytes) {
            uassert(16945,
                    "Exceeded memory limit for $group, but didn't allow external sort."
                    " Pass allowDiskUse:true to opt in.",
                    _allowDiskUse);
            _sortedFiles.push_back(spill());
            _memoryUsageBytes = 0;
        }

        // We release the result document here so that it does not outlive the end of this loop
        // iteration. Not releasing could lead to an array copy when this group follows an unwind.
        auto rootDocument = input.releaseDocument();
        Value id = computeId(rootDocument);

        // Look for the _id value in the map. If it's not there, add a new entry with a blank
        // accumulator. This is done in a somewhat odd way in order to avoid hashing 'id' and
        // looking it up in '_groups' multiple times.
        const size_t oldSize = _groups->size();
        vector<intrusive_ptr<Accumulator>>& group = (*_groups)[id];
        const bool inserted = _groups->size() != oldSize;

        if (inserted) {
            _memoryUsageBytes += id.getApproximateSize();

            // Add the accumulators
            group.reserve(numAccumulators);
            for (auto&& accumulatedField : _accumulatedFields) {
                group.push_back(accumulatedField.makeAccumulator(pExpCtx));
            }
        } else {
            for (auto&& groupObj : group) {
                // subtract old mem usage. New usage added back after processing.
                _memoryUsageBytes -= groupObj->memUsageForSorter();
            }
        }

        /* tickle all the accumulators for the group we found */
        dassert(numAccumulators == group.size());

        for (size_t i = 0; i < numAccumulators; i++) {
            group[i]->process(_accumulatedFields[i].expression->evaluate(rootDocument),
                              _doingMerge);

            _memoryUsageBytes += group[i]->memUsageForSorter();
        }

        if (kDebugBuild && !storageGlobalParams.readOnly) {
            // In debug mode, spill every time we have a duplicate id to stress merge logic.
            if (!inserted &&                 // is a dup
                !pExpCtx->inMongos &&        // can't spill to disk in mongos
                !_allowDiskUse &&            // don't change behavior when testing external sort
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
                    _currentAccumulators.push_back(accumulatedField.makeAccumulator(pExpCtx));
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
    vector<const GroupsMap::value_type*> ptrs;  // using pointers to speed sorting
    ptrs.reserve(_groups->size());
    for (GroupsMap::const_iterator it = _groups->begin(), end = _groups->end(); it != end; ++it) {
        ptrs.push_back(&*it);
    }

    stable_sort(ptrs.begin(), ptrs.end(), SpillSTLComparator(pExpCtx->getValueComparator()));

    SortedFileWriter<Value, Value> writer(SortOptions().TempDir(pExpCtx->tempDir));
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

    _groups->clear();

    return shared_ptr<Sorter<Value, Value>::Iterator>(writer.done());
}

boost::optional<BSONObj> DocumentSourceGroup::findRelevantInputSort() const {
    if (true) {
        // Until streaming $group correctly handles nullish values, the streaming behavior is
        // disabled. See SERVER-23318.
        return boost::none;
    }

    if (!pSource) {
        // Sometimes when performing an explain, or using $group as the merge point, 'pSource' will
        // not be set.
        return boost::none;
    }

    BSONObjSet sorts = pSource->getOutputSorts();

    // 'sorts' is a BSONObjSet. We need to check if our group pattern is compatible with one of the
    // input sort patterns.

    // We will only attempt to take advantage of a sorted input stream if the _id given to the
    // $group contained only FieldPaths or constants. Determine if this is the case, and extract
    // those FieldPaths if it is.
    DepsTracker deps(DepsTracker::MetadataAvailable::kNoMetadata);  // We don't support streaming
                                                                    // based off a text score.
    for (auto&& exp : _idExpressions) {
        if (dynamic_cast<ExpressionConstant*>(exp.get())) {
            continue;
        }
        ExpressionObject* obj;
        if ((obj = dynamic_cast<ExpressionObject*>(exp.get()))) {
            // We can only perform an optimization if there are no operators in the _id expression.
            if (!containsOnlyFieldPathsAndConstants(obj)) {
                return boost::none;
            }
        } else if (!dynamic_cast<ExpressionFieldPath*>(exp.get())) {
            return boost::none;
        }
        exp->addDependencies(&deps);
    }

    if (deps.needWholeDocument) {
        // We don't swap to streaming if we need the entire document, which is likely because of
        // $$ROOT.
        return boost::none;
    }

    if (deps.fields.empty()) {
        // Our _id field is constant, so we should stream, but the input sort we choose is
        // irrelevant since we will output only one document.
        return BSONObj();
    }

    for (auto&& obj : sorts) {
        // Note that a sort order of, e.g., {a: 1, b: 1, c: 1} allows us to do a non-blocking group
        // for every permutation of group by (a, b, c), since we are guaranteed that documents with
        // the same value of (a, b, c) will be consecutive in the input stream, no matter what our
        // _id is.
        std::set<std::string> fieldNames;
        obj.getFieldNames(fieldNames);
        if (fieldNames == deps.fields) {
            return obj;
        }
    }

    return boost::none;
}

BSONObjSet DocumentSourceGroup::getOutputSorts() {
    if (!_initialized) {
        initialize();  // Note this might not finish initializing, but that's OK. We just want to
                       // do some initialization to try to determine if we are streaming or spilled.
                       // False negatives are OK.
    }

    if (!(_streaming || _spilled)) {
        return SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    }

    BSONObjBuilder sortOrder;

    if (_idFieldNames.empty()) {
        if (_spilled) {
            sortOrder.append("_id", 1);
        } else {
            // We have an expression like {_id: "$a"}. Check if this is a FieldPath, and if it is,
            // get the sort order out of it.
            if (auto obj = dynamic_cast<ExpressionFieldPath*>(_idExpressions[0].get())) {
                FieldPath _idSort = obj->getFieldPath();

                sortOrder.append(
                    "_id",
                    _inputSort.getIntField(_idSort.getFieldName(_idSort.getPathLength() - 1)));
            }
        }
    } else if (_streaming) {
        // At this point, we know that _streaming is true, so _id must have only contained
        // ExpressionObjects, ExpressionConstants or ExpressionFieldPaths. We now process each
        // '_idExpression'.

        // We populate 'fieldMap' such that each key is a field the input is sorted by, and the
        // value is where that input field is located within the _id document. For example, if our
        // _id object is {_id: {x: {y: "$a.b"}}}, 'fieldMap' would be: {'a.b': '_id.x.y'}.
        StringMap<std::string> fieldMap;
        for (size_t i = 0; i < _idFieldNames.size(); i++) {
            intrusive_ptr<Expression> exp = _idExpressions[i];
            if (auto obj = dynamic_cast<ExpressionObject*>(exp.get())) {
                // _id is an object containing a nested document, such as: {_id: {x: {y: "$b"}}}.
                getFieldPathMap(obj, "_id." + _idFieldNames[i], &fieldMap);
            } else if (auto fieldPath = dynamic_cast<ExpressionFieldPath*>(exp.get())) {
                FieldPath _idSort = fieldPath->getFieldPath();
                fieldMap[_idSort.getFieldName(_idSort.getPathLength() - 1)] =
                    "_id." + _idFieldNames[i];
            }
        }

        // Because the order of '_inputSort' is important, we go through each field we are sorted on
        // and append it to the BSONObjBuilder in order.
        for (BSONElement sortField : _inputSort) {
            std::string sortString = sortField.fieldNameStringData().toString();

            auto itr = fieldMap.find(sortString);

            // If our sort order is (a, b, c), we could not have converted to a streaming $group if
            // our _id was predicated on (a, c) but not 'b'. Verify that this is true.
            invariant(itr != fieldMap.end());

            sortOrder.append(itr->second, _inputSort.getIntField(sortString));
        }
    } else {
        // We are blocking and have spilled to disk.
        std::vector<std::string> outputSort;
        for (size_t i = 0; i < _idFieldNames.size(); i++) {
            intrusive_ptr<Expression> exp = _idExpressions[i];
            if (auto obj = dynamic_cast<ExpressionObject*>(exp.get())) {
                // _id is an object containing a nested document, such as: {_id: {x: {y: "$b"}}}.
                getFieldPathListForSpilled(obj, "_id." + _idFieldNames[i], &outputSort);
            } else {
                outputSort.push_back("_id." + _idFieldNames[i]);
            }
        }
        for (auto&& field : outputSort) {
            sortOrder.append(field, 1);
        }
    }

    return allPrefixes(sortOrder.obj());
}


Value DocumentSourceGroup::computeId(const Document& root) {
    // If only one expression, return result directly
    if (_idExpressions.size() == 1) {
        Value retValue = _idExpressions[0]->evaluate(root);
        return retValue.missing() ? Value(BSONNULL) : std::move(retValue);
    }

    // Multiple expressions get results wrapped in a vector
    vector<Value> vals;
    vals.reserve(_idExpressions.size());
    for (size_t i = 0; i < _idExpressions.size(); i++) {
        vals.push_back(_idExpressions[i]->evaluate(root));
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
            out.addField(_accumulatedFields[i].fieldName, val);
        }
    }

    return out.freeze();
}

intrusive_ptr<DocumentSource> DocumentSourceGroup::getShardSource() {
    return this;  // No modifications necessary when on shard
}

std::list<intrusive_ptr<DocumentSource>> DocumentSourceGroup::getMergeSources() {
    intrusive_ptr<DocumentSourceGroup> pMerger(new DocumentSourceGroup(pExpCtx));
    pMerger->setDoingMerge(true);

    VariablesParseState vps = pExpCtx->variablesParseState;
    /* the merger will use the same grouping key */
    pMerger->setIdExpression(ExpressionFieldPath::parse(pExpCtx, "$$ROOT._id", vps));

    for (auto&& accumulatedField : _accumulatedFields) {
        // The merger's output field names will be the same, as will the accumulator factories.
        // However, for some accumulators, the expression to be accumulated will be different. The
        // original accumulator may be collecting an expression based on a field expression or
        // constant.  Here, we accumulate the output of the same name from the prior group.
        auto copiedAccumuledField = accumulatedField;
        copiedAccumuledField.expression =
            ExpressionFieldPath::parse(pExpCtx, "$$ROOT." + accumulatedField.fieldName, vps);
        pMerger->addAccumulator(copiedAccumuledField);
    }

    return {pMerger};
}
}

#include "mongo/db/sorter/sorter.cpp"
// Explicit instantiation unneeded since we aren't exposing Sorter outside of this file.
