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
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::pair;
using std::vector;

REGISTER_DOCUMENT_SOURCE(group, DocumentSourceGroup::createFromBson);

const char* DocumentSourceGroup::getSourceName() const {
    return "$group";
}

boost::optional<Document> DocumentSourceGroup::getNext() {
    pExpCtx->checkForInterrupt();

    if (!_initialized)
        initialize();

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

boost::optional<Document> DocumentSourceGroup::getNextSpilled() {
    // We aren't streaming, and we have spilled to disk.
    if (!_sorterIterator)
        return boost::none;

    _currentId = _firstPartOfNextGroup.first;
    const size_t numAccumulators = vpAccumulatorFactory.size();
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

    return makeDocument(_currentId, _currentAccumulators, pExpCtx->inShard);
}

boost::optional<Document> DocumentSourceGroup::getNextStandard() {
    // Not spilled, and not streaming.
    if (_groups->empty())
        return boost::none;

    Document out = makeDocument(groupsIterator->first, groupsIterator->second, pExpCtx->inShard);

    if (++groupsIterator == _groups->end())
        dispose();

    return out;
}

boost::optional<Document> DocumentSourceGroup::getNextStreaming() {
    // Streaming optimization is active.
    if (!_firstDocOfNextGroup) {
        dispose();
        return boost::none;
    }

    Value id;
    do {
        // Add to the current accumulator(s).
        for (size_t i = 0; i < _currentAccumulators.size(); i++) {
            _currentAccumulators[i]->process(vpExpression[i]->evaluate(_variables.get()),
                                             _doingMerge);
        }

        // Release our references to the previous input document before asking for the next. This
        // makes operations like $unwind more efficient.
        _variables->clearRoot();
        _firstDocOfNextGroup = boost::none;

        // Retrieve the next document.
        _firstDocOfNextGroup = pSource->getNext();
        if (!_firstDocOfNextGroup) {
            break;
        }

        _variables->setRoot(*_firstDocOfNextGroup);

        // Compute the id. If it does not match _currentId, we will exit the loop, leaving
        // _firstDocOfNextGroup set for the next time getNext() is called.
        id = computeId(_variables.get());
    } while (pExpCtx->getValueComparator().evaluate(_currentId == id));

    Document out = makeDocument(_currentId, _currentAccumulators, pExpCtx->inShard);
    _currentId = std::move(id);

    return out;
}

void DocumentSourceGroup::dispose() {
    // Free our resources.
    GroupsMap().swap(*_groups);
    _sorterIterator.reset();

    // Make us look done.
    groupsIterator = _groups->end();

    _firstDocOfNextGroup = boost::none;

    // Free our source's resources.
    pSource->dispose();
}

intrusive_ptr<DocumentSource> DocumentSourceGroup::optimize() {
    // TODO: If all _idExpressions are ExpressionConstants after optimization, then we know there
    // will be only one group. We should take advantage of that to avoid going through the hash
    // table.
    for (size_t i = 0; i < _idExpressions.size(); i++) {
        _idExpressions[i] = _idExpressions[i]->optimize();
    }

    for (size_t i = 0; i < vFieldName.size(); i++) {
        vpExpression[i] = vpExpression[i]->optimize();
    }

    return this;
}

void DocumentSourceGroup::doInjectExpressionContext() {
    // Groups map must respect new comparator.
    _groups = pExpCtx->getValueComparator().makeUnorderedValueMap<Accumulators>();

    for (auto&& idExpr : _idExpressions) {
        idExpr->injectExpressionContext(pExpCtx);
    }

    for (auto&& expr : vpExpression) {
        expr->injectExpressionContext(pExpCtx);
    }

    for (auto&& accum : _currentAccumulators) {
        accum->injectExpressionContext(pExpCtx);
    }
}

Value DocumentSourceGroup::serialize(bool explain) const {
    MutableDocument insides;

    // Add the _id.
    if (_idFieldNames.empty()) {
        invariant(_idExpressions.size() == 1);
        insides["_id"] = _idExpressions[0]->serialize(explain);
    } else {
        // Decomposed document case.
        invariant(_idExpressions.size() == _idFieldNames.size());
        MutableDocument md;
        for (size_t i = 0; i < _idExpressions.size(); i++) {
            md[_idFieldNames[i]] = _idExpressions[i]->serialize(explain);
        }
        insides["_id"] = md.freezeToValue();
    }

    // Add the remaining fields.
    const size_t n = vFieldName.size();
    for (size_t i = 0; i < n; ++i) {
        intrusive_ptr<Accumulator> accum = vpAccumulatorFactory[i]();
        insides[vFieldName[i]] =
            Value(DOC(accum->getOpName() << vpExpression[i]->serialize(explain)));
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

DocumentSource::GetDepsReturn DocumentSourceGroup::getDependencies(DepsTracker* deps) const {
    // add the _id
    for (size_t i = 0; i < _idExpressions.size(); i++) {
        _idExpressions[i]->addDependencies(deps);
    }

    // add the rest
    const size_t n = vFieldName.size();
    for (size_t i = 0; i < n; ++i) {
        vpExpression[i]->addDependencies(deps);
    }

    return EXHAUSTIVE_ALL;
}

intrusive_ptr<DocumentSourceGroup> DocumentSourceGroup::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx) {
    intrusive_ptr<DocumentSourceGroup> source(new DocumentSourceGroup(pExpCtx));
    source->injectExpressionContext(pExpCtx);
    return source;
}

DocumentSourceGroup::DocumentSourceGroup(const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx),
      _doingMerge(false),
      _maxMemoryUsageBytes(100 * 1024 * 1024),
      _inputSort(BSONObj()),
      _streaming(false),
      _initialized(false),
      _spilled(false),
      _extSortAllowed(pExpCtx->extSortAllowed && !pExpCtx->inRouter) {}

void DocumentSourceGroup::addAccumulator(const std::string& fieldName,
                                         Accumulator::Factory accumulatorFactory,
                                         const intrusive_ptr<Expression>& pExpression) {
    vFieldName.push_back(fieldName);
    vpAccumulatorFactory.push_back(accumulatorFactory);
    vpExpression.push_back(pExpression);
}

intrusive_ptr<DocumentSource> DocumentSourceGroup::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(15947, "a group's fields must be specified in an object", elem.type() == Object);

    intrusive_ptr<DocumentSourceGroup> pGroup(DocumentSourceGroup::create(pExpCtx));

    BSONObj groupObj(elem.Obj());
    BSONObjIterator groupIterator(groupObj);
    VariablesIdGenerator idGenerator;
    VariablesParseState vps(&idGenerator);
    while (groupIterator.more()) {
        BSONElement groupField(groupIterator.next());
        const char* pFieldName = groupField.fieldName();

        if (str::equals(pFieldName, "_id")) {
            uassert(
                15948, "a group's _id may only be specified once", pGroup->_idExpressions.empty());
            pGroup->parseIdExpression(groupField, vps);
            invariant(!pGroup->_idExpressions.empty());
        } else if (str::equals(pFieldName, "$doingMerge")) {
            massert(17030, "$doingMerge should be true if present", groupField.Bool());

            pGroup->setDoingMerge(true);
        } else {
            /*
              Treat as a projection field with the additional ability to
              add aggregation operators.
            */
            uassert(
                16414,
                str::stream() << "the group aggregate field name '" << pFieldName
                              << "' cannot be used because $group's field names cannot contain '.'",
                !str::contains(pFieldName, '.'));

            uassert(15950,
                    str::stream() << "the group aggregate field name '" << pFieldName
                                  << "' cannot be an operator name",
                    pFieldName[0] != '$');

            uassert(15951,
                    str::stream() << "the group aggregate field '" << pFieldName
                                  << "' must be defined as an expression inside an object",
                    groupField.type() == Object);

            BSONObj subField(groupField.Obj());
            BSONObjIterator subIterator(subField);
            size_t subCount = 0;
            for (; subIterator.more(); ++subCount) {
                BSONElement subElement(subIterator.next());

                auto name = subElement.fieldNameStringData();
                Accumulator::Factory factory = Accumulator::getFactory(name);
                intrusive_ptr<Expression> pGroupExpr;
                BSONType elementType = subElement.type();
                if (elementType == Object) {
                    pGroupExpr = Expression::parseObject(subElement.Obj(), vps);
                } else if (elementType == Array) {
                    uasserted(15953,
                              str::stream() << "aggregating group operators are unary (" << name
                                            << ")");
                } else { /* assume its an atomic single operand */
                    pGroupExpr = Expression::parseOperand(subElement, vps);
                }

                pGroup->addAccumulator(pFieldName, factory, pGroupExpr);
            }

            uassert(15954,
                    str::stream() << "the computed aggregate '" << pFieldName
                                  << "' must specify exactly one operator",
                    subCount == 1);
        }
    }

    uassert(15955, "a group specification must include an _id", !pGroup->_idExpressions.empty());

    pGroup->_variables.reset(new Variables(idGenerator.getIdCount()));

    return pGroup;
}

namespace {

using GroupsMap = DocumentSourceGroup::GroupsMap;

class SorterComparator {
public:
    typedef pair<Value, Value> Data;
    int operator()(const Data& lhs, const Data& rhs) const {
        return Value::compare(lhs.first, rhs.first);
    }
};

class SpillSTLComparator {
public:
    bool operator()(const GroupsMap::value_type* lhs, const GroupsMap::value_type* rhs) const {
        return Value::compare(lhs->first, rhs->first) < 0;
    }
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

void DocumentSourceGroup::initialize() {
    _initialized = true;
    const size_t numAccumulators = vpAccumulatorFactory.size();

    boost::optional<BSONObj> inputSort = findRelevantInputSort();
    if (inputSort) {
        // We can convert to streaming.
        _streaming = true;
        _inputSort = *inputSort;

        // Set up accumulators.
        _currentAccumulators.reserve(numAccumulators);
        for (size_t i = 0; i < numAccumulators; i++) {
            _currentAccumulators.push_back(vpAccumulatorFactory[i]());
            _currentAccumulators.back()->injectExpressionContext(pExpCtx);
        }

        // We only need to load the first document.
        _firstDocOfNextGroup = pSource->getNext();

        if (!_firstDocOfNextGroup) {
            return;
        }

        _variables->setRoot(*_firstDocOfNextGroup);

        // Compute the _id value.
        _currentId = computeId(_variables.get());
        return;
    }

    dassert(numAccumulators == vpExpression.size());

    // pushed to on spill()
    vector<shared_ptr<Sorter<Value, Value>::Iterator>> sortedFiles;
    int memoryUsageBytes = 0;

    // This loop consumes all input from pSource and buckets it based on pIdExpression.
    while (boost::optional<Document> input = pSource->getNext()) {
        if (memoryUsageBytes > _maxMemoryUsageBytes) {
            uassert(16945,
                    "Exceeded memory limit for $group, but didn't allow external sort."
                    " Pass allowDiskUse:true to opt in.",
                    _extSortAllowed);
            sortedFiles.push_back(spill());
            memoryUsageBytes = 0;
        }

        _variables->setRoot(*input);

        /* get the _id value */
        Value id = computeId(_variables.get());

        /*
          Look for the _id value in the map; if it's not there, add a
          new entry with a blank accumulator.
        */
        const size_t oldSize = _groups->size();
        vector<intrusive_ptr<Accumulator>>& group = (*_groups)[id];
        const bool inserted = _groups->size() != oldSize;

        if (inserted) {
            memoryUsageBytes += id.getApproximateSize();

            // Add the accumulators
            group.reserve(numAccumulators);
            for (size_t i = 0; i < numAccumulators; i++) {
                group.push_back(vpAccumulatorFactory[i]());
                group.back()->injectExpressionContext(pExpCtx);
            }
        } else {
            for (size_t i = 0; i < numAccumulators; i++) {
                // subtract old mem usage. New usage added back after processing.
                memoryUsageBytes -= group[i]->memUsageForSorter();
            }
        }

        /* tickle all the accumulators for the group we found */
        dassert(numAccumulators == group.size());
        for (size_t i = 0; i < numAccumulators; i++) {
            group[i]->process(vpExpression[i]->evaluate(_variables.get()), _doingMerge);
            memoryUsageBytes += group[i]->memUsageForSorter();
        }

        // We are done with the ROOT document so release it.
        _variables->clearRoot();

        if (kDebugBuild && !storageGlobalParams.readOnly) {
            // In debug mode, spill every time we have a duplicate id to stress merge logic.
            if (!inserted  // is a dup
                &&
                !pExpCtx->inRouter  // can't spill to disk in router
                &&
                !_extSortAllowed  // don't change behavior when testing external sort
                &&
                sortedFiles.size() < 20  // don't open too many FDs
                ) {
                sortedFiles.push_back(spill());
            }
        }
    }

    // These blocks do any final steps necessary to prepare to output results.
    if (!sortedFiles.empty()) {
        _spilled = true;
        if (!_groups->empty()) {
            sortedFiles.push_back(spill());
        }

        // We won't be using groups again so free its memory.
        GroupsMap().swap(*_groups);

        _sorterIterator.reset(
            Sorter<Value, Value>::Iterator::merge(sortedFiles, SortOptions(), SorterComparator()));

        // prepare current to accumulate data
        _currentAccumulators.reserve(numAccumulators);
        for (size_t i = 0; i < numAccumulators; i++) {
            _currentAccumulators.push_back(vpAccumulatorFactory[i]());
            _currentAccumulators.back()->injectExpressionContext(pExpCtx);
        }

        verify(_sorterIterator->more());  // we put data in, we should get something out.
        _firstPartOfNextGroup = _sorterIterator->next();
    } else {
        // start the group iterator
        groupsIterator = _groups->begin();
    }
}

shared_ptr<Sorter<Value, Value>::Iterator> DocumentSourceGroup::spill() {
    vector<const GroupsMap::value_type*> ptrs;  // using pointers to speed sorting
    ptrs.reserve(_groups->size());
    for (GroupsMap::const_iterator it = _groups->begin(), end = _groups->end(); it != end; ++it) {
        ptrs.push_back(&*it);
    }

    stable_sort(ptrs.begin(), ptrs.end(), SpillSTLComparator());

    SortedFileWriter<Value, Value> writer(SortOptions().TempDir(pExpCtx->tempDir));
    switch (vpAccumulatorFactory.size()) {  // same as ptrs[i]->second.size() for all i.
        case 0:                             // no values, essentially a distinct
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
        initialize();
    }

    if (!(_streaming || _spilled)) {
        return BSONObjSet();
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


void DocumentSourceGroup::parseIdExpression(BSONElement groupField,
                                            const VariablesParseState& vps) {
    if (groupField.type() == Object && !groupField.Obj().isEmpty()) {
        // {_id: {}} is treated as grouping on a constant, not an expression

        const BSONObj idKeyObj = groupField.Obj();
        if (idKeyObj.firstElementFieldName()[0] == '$') {
            // grouping on a $op expression
            _idExpressions.push_back(Expression::parseObject(idKeyObj, vps));
        } else {
            // grouping on an "artificial" object. Rather than create the object for each input
            // in initialize(), instead group on the output of the raw expressions. The artificial
            // object will be created at the end in makeDocument() while outputting results.
            BSONForEach(field, idKeyObj) {
                uassert(17390,
                        "$group does not support inclusion-style expressions",
                        !field.isNumber() && field.type() != Bool);

                _idFieldNames.push_back(field.fieldName());
                _idExpressions.push_back(Expression::parseOperand(field, vps));
            }
        }
    } else if (groupField.type() == String && groupField.valuestr()[0] == '$') {
        // grouping on a field path.
        _idExpressions.push_back(ExpressionFieldPath::parse(groupField.str(), vps));
    } else {
        // constant id - single group
        _idExpressions.push_back(ExpressionConstant::create(pExpCtx, Value(groupField)));
    }
}

Value DocumentSourceGroup::computeId(Variables* vars) {
    // If only one expression, return result directly
    if (_idExpressions.size() == 1) {
        Value retValue = _idExpressions[0]->evaluate(vars);
        return retValue.missing() ? Value(BSONNULL) : std::move(retValue);
    }

    // Multiple expressions get results wrapped in a vector
    vector<Value> vals;
    vals.reserve(_idExpressions.size());
    for (size_t i = 0; i < _idExpressions.size(); i++) {
        vals.push_back(_idExpressions[i]->evaluate(vars));
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
    const size_t n = vFieldName.size();
    MutableDocument out(1 + n);

    /* add the _id field */
    out.addField("_id", expandId(id));

    /* add the rest of the fields */
    for (size_t i = 0; i < n; ++i) {
        Value val = accums[i]->getValue(mergeableOutput);
        if (val.missing()) {
            // we return null in this case so return objects are predictable
            out.addField(vFieldName[i], Value(BSONNULL));
        } else {
            out.addField(vFieldName[i], val);
        }
    }

    return out.freeze();
}

intrusive_ptr<DocumentSource> DocumentSourceGroup::getShardSource() {
    return this;  // No modifications necessary when on shard
}

intrusive_ptr<DocumentSource> DocumentSourceGroup::getMergeSource() {
    intrusive_ptr<DocumentSourceGroup> pMerger(DocumentSourceGroup::create(pExpCtx));
    pMerger->setDoingMerge(true);

    VariablesIdGenerator idGenerator;
    VariablesParseState vps(&idGenerator);
    /* the merger will use the same grouping key */
    pMerger->_idExpressions.push_back(ExpressionFieldPath::parse("$$ROOT._id", vps));

    const size_t n = vFieldName.size();
    for (size_t i = 0; i < n; ++i) {
        /*
          The merger's output field names will be the same, as will the
          accumulator factories.  However, for some accumulators, the
          expression to be accumulated will be different.  The original
          accumulator may be collecting an expression based on a field
          expression or constant.  Here, we accumulate the output of the
          same name from the prior group.
        */
        pMerger->addAccumulator(vFieldName[i],
                                vpAccumulatorFactory[i],
                                ExpressionFieldPath::parse("$$ROOT." + vFieldName[i], vps));
    }

    pMerger->_variables.reset(new Variables(idGenerator.getIdCount()));

    return pMerger;
}
}

#include "mongo/db/sorter/sorter.cpp"
// Explicit instantiation unneeded since we aren't exposing Sorter outside of this file.
