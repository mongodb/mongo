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

#include "mongo/pch.h"


#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {
    const char DocumentSourceGroup::groupName[] = "$group";

    const char *DocumentSourceGroup::getSourceName() const {
        return groupName;
    }

    boost::optional<Document> DocumentSourceGroup::getNext() {
        pExpCtx->checkForInterrupt();

        if (!populated)
            populate();

        if (_spilled) {
            if (!_sorterIterator)
                return boost::none;

            const size_t numAccumulators = vpAccumulatorFactory.size();
            for (size_t i=0; i < numAccumulators; i++) {
                _currentAccumulators[i]->reset(); // prep accumulators for a new group
            }

            _currentId = _firstPartOfNextGroup.first;
            while (_currentId == _firstPartOfNextGroup.first) {
                // Inside of this loop, _firstPartOfNextGroup is the current data being processed.
                // At loop exit, it is the first value to be processed in the next group.

                switch (numAccumulators) { // mirrors switch in spill()
                case 0: // no Accumulators so no Values
                    break;

                case 1: // single accumulators serialize as a single Value
                    _currentAccumulators[0]->process(_firstPartOfNextGroup.second,
                                                     /*merging=*/true);
                    break;

                default: { // multiple accumulators serialize as an array
                    const vector<Value>& accumulatorStates =
                        _firstPartOfNextGroup.second.getArray();
                    for (size_t i=0; i < numAccumulators; i++) {
                        _currentAccumulators[i]->process(accumulatorStates[i],
                                                         /*merging=*/true);
                    }
                    break;
                }
                }

                if (!_sorterIterator->more()) {
                    dispose();
                    break;
                }

                _firstPartOfNextGroup = _sorterIterator->next();
            }

            return makeDocument(_currentId, _currentAccumulators, pExpCtx->inShard);

        } else {
            if (groups.empty())
                return boost::none;

            Document out = makeDocument(groupsIterator->first,
                                        groupsIterator->second,
                                        pExpCtx->inShard);

            if (++groupsIterator == groups.end())
                dispose();

            return out;
        }
    }

    void DocumentSourceGroup::dispose() {
        // free our resources
        GroupsMap().swap(groups);
        _sorterIterator.reset();

        // make us look done
        groupsIterator = groups.end();

        // free our source's resources
        pSource->dispose();
    }

    Value DocumentSourceGroup::serialize(bool explain) const {
        MutableDocument insides;

        // add the _id
        insides["_id"] = pIdExpression->serialize();

        // add the remaining fields
        const size_t n = vFieldName.size();
        for(size_t i = 0; i < n; ++i) {
            intrusive_ptr<Accumulator> accum = vpAccumulatorFactory[i]();
            insides[vFieldName[i]] = Value(DOC(accum->getOpName() << vpExpression[i]->serialize()));
        }

        if (_doingMerge) {
            // This makes the output unparsable (with error) on pre 2.6 shards, but it will never
            // be sent to old shards when this flag is true since they can't do a merge anyway.

            insides["$doingMerge"] = Value(true);
        }

        return Value(DOC(getSourceName() << insides.freeze()));
    }

    DocumentSource::GetDepsReturn DocumentSourceGroup::getDependencies(set<string>& deps) const {
        // add the _id
        pIdExpression->addDependencies(deps);

        // add the rest
        const size_t n = vFieldName.size();
        for(size_t i = 0; i < n; ++i) {
            vpExpression[i]->addDependencies(deps);
        }

        return EXHAUSTIVE;
    }

    intrusive_ptr<DocumentSourceGroup> DocumentSourceGroup::create(
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        intrusive_ptr<DocumentSourceGroup> pSource(
            new DocumentSourceGroup(pExpCtx));
        return pSource;
    }

    DocumentSourceGroup::DocumentSourceGroup(const intrusive_ptr<ExpressionContext>& pExpCtx)
        : SplittableDocumentSource(pExpCtx)
        , populated(false)
        , _doingMerge(false)
        , _spilled(false)
        , _extSortAllowed(pExpCtx->extSortAllowed && !pExpCtx->inRouter)
        , _maxMemoryUsageBytes(100*1024*1024)
    {}

    void DocumentSourceGroup::addAccumulator(
            const std::string& fieldName,
            intrusive_ptr<Accumulator> (*pAccumulatorFactory)(),
            const intrusive_ptr<Expression> &pExpression) {
        vFieldName.push_back(fieldName);
        vpAccumulatorFactory.push_back(pAccumulatorFactory);
        vpExpression.push_back(pExpression);
    }


    struct GroupOpDesc {
        const char* name;
        intrusive_ptr<Accumulator> (*factory)();
    };

    static int GroupOpDescCmp(const void *pL, const void *pR) {
        return strcmp(((const GroupOpDesc *)pL)->name,
                      ((const GroupOpDesc *)pR)->name);
    }

    /*
      Keep these sorted alphabetically so we can bsearch() them using
      GroupOpDescCmp() above.
    */
    static const GroupOpDesc GroupOpTable[] = {
        {"$addToSet", AccumulatorAddToSet::create},
        {"$avg", AccumulatorAvg::create},
        {"$first", AccumulatorFirst::create},
        {"$last", AccumulatorLast::create},
        {"$max", AccumulatorMinMax::createMax},
        {"$min", AccumulatorMinMax::createMin},
        {"$push", AccumulatorPush::create},
        {"$sum", AccumulatorSum::create},
    };

    static const size_t NGroupOp = sizeof(GroupOpTable)/sizeof(GroupOpTable[0]);

    intrusive_ptr<DocumentSource> DocumentSourceGroup::createFromBson(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        uassert(15947, "a group's fields must be specified in an object",
                pBsonElement->type() == Object);

        intrusive_ptr<DocumentSourceGroup> pGroup(
            DocumentSourceGroup::create(pExpCtx));
        bool idSet = false;

        BSONObj groupObj(pBsonElement->Obj());
        BSONObjIterator groupIterator(groupObj);
        while(groupIterator.more()) {
            BSONElement groupField(groupIterator.next());
            const char *pFieldName = groupField.fieldName();

            if (str::equals(pFieldName, "_id")) {
                uassert(15948, "a group's _id may only be specified once",
                        !idSet);

                BSONType groupType = groupField.type();

                if (groupType == Object) {
                    /*
                      Use the projection-like set of field paths to create the
                      group-by key.
                    */
                    Expression::ObjectCtx oCtx(Expression::ObjectCtx::DOCUMENT_OK);
                    intrusive_ptr<Expression> pId(
                        Expression::parseObject(&groupField, &oCtx));

                    pGroup->setIdExpression(pId);
                    idSet = true;
                }
                else if (groupType == String) {
                    const string groupString = groupField.str();
                    if (!groupString.empty() && groupString[0] == '$') {
                        pGroup->setIdExpression(ExpressionFieldPath::parse(groupString));
                        idSet = true;
                    }
                }

                if (!idSet) {
                    // constant id - single group
                    pGroup->setIdExpression(ExpressionConstant::create(Value(groupField)));
                    idSet = true;
                }
            }
            else if (str::equals(pFieldName, "$doingMerge")) {
                massert(17030, "$doingMerge should be true if present",
                        groupField.Bool());

                pGroup->setDoingMerge(true);
            }
            else {
                /*
                  Treat as a projection field with the additional ability to
                  add aggregation operators.
                */
                uassert(16414, str::stream() <<
                        "the group aggregate field name '" << pFieldName <<
                        "' cannot be used because $group's field names cannot contain '.'",
                        !str::contains(pFieldName, '.') );

                uassert(15950, str::stream() <<
                        "the group aggregate field name '" <<
                        pFieldName << "' cannot be an operator name",
                        pFieldName[0] != '$');

                uassert(15951, str::stream() <<
                        "the group aggregate field '" << pFieldName <<
                        "' must be defined as an expression inside an object",
                        groupField.type() == Object);

                BSONObj subField(groupField.Obj());
                BSONObjIterator subIterator(subField);
                size_t subCount = 0;
                for(; subIterator.more(); ++subCount) {
                    BSONElement subElement(subIterator.next());

                    /* look for the specified operator */
                    GroupOpDesc key;
                    key.name = subElement.fieldName();
                    const GroupOpDesc *pOp =
                        (const GroupOpDesc *)bsearch(
                              &key, GroupOpTable, NGroupOp, sizeof(GroupOpDesc),
                                      GroupOpDescCmp);

                    uassert(15952, str::stream() << "unknown group operator '" << key.name << "'",
                            pOp);

                    intrusive_ptr<Expression> pGroupExpr;

                    BSONType elementType = subElement.type();
                    if (elementType == Object) {
                        Expression::ObjectCtx oCtx(
                            Expression::ObjectCtx::DOCUMENT_OK);
                        pGroupExpr = Expression::parseObject(
                            &subElement, &oCtx);
                    }
                    else if (elementType == Array) {
                        uasserted(15953, str::stream()
                                << "aggregating group operators are unary (" << key.name << ")");
                    }
                    else { /* assume its an atomic single operand */
                        pGroupExpr = Expression::parseOperand(subElement);
                    }

                    pGroup->addAccumulator(pFieldName, pOp->factory, pGroupExpr);
                }

                uassert(15954, str::stream() <<
                        "the computed aggregate '" <<
                        pFieldName << "' must specify exactly one operator",
                        subCount == 1);
            }
        }

        uassert(15955, "a group specification must include an _id", idSet);

        return pGroup;
    }

    namespace {
        class SorterComparator {
        public:
            typedef pair<Value, Value> Data;
            int operator() (const Data& lhs, const Data& rhs) const {
                return Value::compare(lhs.first, rhs.first);
            }
        };
    }

    void DocumentSourceGroup::populate() {
        const size_t numAccumulators = vpAccumulatorFactory.size();
        dassert(numAccumulators == vpExpression.size());

        // pushed to on spill()
        vector<shared_ptr<Sorter<Value, Value>::Iterator> > sortedFiles;
        int memoryUsageBytes = 0;

        // This loop consumes all input from pSource and buckets it based on pIdExpression.
        while (boost::optional<Document> input = pSource->getNext()) {
            if (memoryUsageBytes > _maxMemoryUsageBytes) {
                uassert(16945, "Exceeded memory limit for $group, but didn't allow external sort",
                        _extSortAllowed);
                sortedFiles.push_back(spill());
                memoryUsageBytes = 0;
            }

            const Variables vars(*input);

            /* get the _id value */
            Value id = pIdExpression->evaluate(vars);

            /* treat missing values the same as NULL SERVER-4674 */
            if (id.missing())
                id = Value(BSONNULL);

            /*
              Look for the _id value in the map; if it's not there, add a
              new entry with a blank accumulator.
            */
            const size_t oldSize = groups.size();
            vector<intrusive_ptr<Accumulator> >& group = groups[id];
            const bool inserted = groups.size() != oldSize;

            if (inserted) {
                memoryUsageBytes += id.getApproximateSize();

                // Add the accumulators
                group.reserve(numAccumulators);
                for (size_t i = 0; i < numAccumulators; i++) {
                    group.push_back(vpAccumulatorFactory[i]());
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
                group[i]->process(vpExpression[i]->evaluate(vars), _doingMerge);
                memoryUsageBytes += group[i]->memUsageForSorter();
            }

            DEV {
                // In debug mode, spill every time we have a duplicate id to stress merge logic.
                if (!inserted // is a dup
                        && !pExpCtx->inRouter // can't spill to disk in router
                        && !_extSortAllowed // don't change behavior when testing external sort
                        && sortedFiles.size() < 20 // don't open too many FDs
                        ) {
                    sortedFiles.push_back(spill());
                }
            }
        }

        // These blocks do any final steps necessary to prepare to output results.
        if (!sortedFiles.empty()) {
            _spilled = true;
            if (!groups.empty()) {
                sortedFiles.push_back(spill());
            }

            // We won't be using groups again so free its memory.
            GroupsMap().swap(groups);

            _sorterIterator.reset(
                    Sorter<Value,Value>::Iterator::merge(
                        sortedFiles, SortOptions(), SorterComparator()));

            // prepare current to accumulate data
            _currentAccumulators.reserve(numAccumulators);
            for (size_t i = 0; i < numAccumulators; i++) {
                _currentAccumulators.push_back(vpAccumulatorFactory[i]());
            }

            verify(_sorterIterator->more()); // we put data in, we should get something out.
            _firstPartOfNextGroup = _sorterIterator->next();
        } else {
            // start the group iterator
            groupsIterator = groups.begin();
        }

        populated = true;
    }

    class DocumentSourceGroup::SpillSTLComparator {
    public:
        bool operator() (const GroupsMap::value_type* lhs, const GroupsMap::value_type* rhs) const {
            return Value::compare(lhs->first, rhs->first) < 0;
        }
    };

    shared_ptr<Sorter<Value, Value>::Iterator> DocumentSourceGroup::spill() {
        vector<const GroupsMap::value_type*> ptrs; // using pointers to speed sorting
        ptrs.reserve(groups.size());
        for (GroupsMap::const_iterator it=groups.begin(), end=groups.end(); it != end; ++it) {
            ptrs.push_back(&*it);
        }

        stable_sort(ptrs.begin(), ptrs.end(), SpillSTLComparator());

        SortedFileWriter<Value, Value> writer;
        switch (vpAccumulatorFactory.size()) { // same as ptrs[i]->second.size() for all i.
        case 0: // no values, essentially a distinct
            for (size_t i=0; i < ptrs.size(); i++) {
                writer.addAlreadySorted(ptrs[i]->first, Value());
            }
            break;

        case 1: // just one value, use optimized serialization as single Value
            for (size_t i=0; i < ptrs.size(); i++) {
                writer.addAlreadySorted(ptrs[i]->first,
                                        ptrs[i]->second[0]->getValue(/*toBeMerged=*/true));
            }
            break;

        default: // multiple values, serialize as array-typed Value
            for (size_t i=0; i < ptrs.size(); i++) {
                vector<Value> accums;
                for (size_t j=0; j < ptrs[i]->second.size(); j++) {
                    accums.push_back(ptrs[i]->second[j]->getValue(/*toBeMerged=*/true));
                }
                writer.addAlreadySorted(ptrs[i]->first, Value::consume(accums));
            }
            break;
        }

        groups.clear();

        return shared_ptr<Sorter<Value, Value>::Iterator>(writer.done());
    }

    Document DocumentSourceGroup::makeDocument(const Value& id,
                                               const Accumulators& accums,
                                               bool mergeableOutput) {
        const size_t n = vFieldName.size();
        MutableDocument out (1 + n);

        /* add the _id field */
        out.addField("_id", id);

        /* add the rest of the fields */
        for(size_t i = 0; i < n; ++i) {
            Value val = accums[i]->getValue(mergeableOutput);
            if (val.missing()) {
                // we return null in this case so return objects are predictable
                out.addField(vFieldName[i], Value(BSONNULL));
            }
            else {
                out.addField(vFieldName[i], val);
            }
        }

        return out.freeze();
    }

    intrusive_ptr<DocumentSource> DocumentSourceGroup::getShardSource() {
        return this; // No modifications necessary when on shard
    }

    intrusive_ptr<DocumentSource> DocumentSourceGroup::getRouterSource() {
        intrusive_ptr<DocumentSourceGroup> pMerger(DocumentSourceGroup::create(pExpCtx));
        pMerger->setDoingMerge(true);

        /* the merger will use the same grouping key */
        pMerger->setIdExpression(ExpressionFieldPath::parse("$$ROOT._id"));

        const size_t n = vFieldName.size();
        for(size_t i = 0; i < n; ++i) {
            /*
              The merger's output field names will be the same, as will the
              accumulator factories.  However, for some accumulators, the
              expression to be accumulated will be different.  The original
              accumulator may be collecting an expression based on a field
              expression or constant.  Here, we accumulate the output of the
              same name from the prior group.
            */
            pMerger->addAccumulator(
                vFieldName[i], vpAccumulatorFactory[i],
                ExpressionFieldPath::parse("$$ROOT." + vFieldName[i]));
        }

        return pMerger;
    }
}

#include "db/sorter/sorter.cpp"
// Explicit instantiation unneeded since we aren't exposing Sorter outside of this file.
