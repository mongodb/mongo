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
*/

#include "pch.h"

#include "db/pipeline/document_source.h"

#include "db/jsobj.h"
#include "db/pipeline/accumulator.h"
#include "db/pipeline/document.h"
#include "db/pipeline/expression.h"
#include "db/pipeline/expression_context.h"
#include "db/pipeline/value.h"

namespace mongo {
    const char DocumentSourceGroup::groupName[] = "$group";

    DocumentSourceGroup::~DocumentSourceGroup() {
    }

    const char *DocumentSourceGroup::getSourceName() const {
        return groupName;
    }

    bool DocumentSourceGroup::eof() {
        if (!populated)
            populate();

        return (groupsIterator == groups.end());
    }

    bool DocumentSourceGroup::advance() {
        DocumentSource::advance(); // check for interrupts

        if (!populated)
            populate();

        verify(groupsIterator != groups.end());

        ++groupsIterator;
        if (groupsIterator == groups.end()) {
            dispose();
            return false;
        }

        return true;
    }

    Document DocumentSourceGroup::getCurrent() {
        if (!populated)
            populate();

        return makeDocument(*groupsIterator, pExpCtx->getInShard());
    }

    void DocumentSourceGroup::dispose() {
        GroupsMap().swap(groups);
        groupsIterator = groups.end();

        pSource->dispose();
    }

    void DocumentSourceGroup::sourceToBson(BSONObjBuilder* pBuilder, bool explain) const {
        MutableDocument insides;

        /* add the _id */
        insides["_id"] = pIdExpression->serialize();

        /* add the remaining fields */
        const size_t n = vFieldName.size();
        for(size_t i = 0; i < n; ++i) {
            intrusive_ptr<Accumulator> accum = vpAccumulatorFactory[i]();
            insides[vFieldName[i]] = Value(
                    DOC(accum->getOpName() << vpExpression[i]->serialize()));
        }

        *pBuilder << groupName << insides.freeze();
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

    DocumentSourceGroup::DocumentSourceGroup(
        const intrusive_ptr<ExpressionContext> &pExpCtx):
        SplittableDocumentSource(pExpCtx),
        populated(false),
        pIdExpression(),
        groups(),
        vFieldName(),
        vpAccumulatorFactory(),
        vpExpression() {
    }

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
                        pGroupExpr = Expression::parseOperand(&subElement);
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

    void DocumentSourceGroup::populate() {
        const size_t numAccumulators = vpAccumulatorFactory.size();
        dassert(numAccumulators == vpExpression.size());

        const bool mergeInputs = pExpCtx->getDoingMerge();

        for (bool hasNext = !pSource->eof(); hasNext; hasNext = pSource->advance()) {
            const Document input = pSource->getCurrent();
            const Variables vars (input);

            /* get the _id value */
            Value id = pIdExpression->evaluate(vars);

            /* treat missing values the same as NULL SERVER-4674 */
            if (id.missing())
                id = Value(BSONNULL);

            /*
              Look for the _id value in the map; if it's not there, add a
              new entry with a blank accumulator.
            */
            vector<intrusive_ptr<Accumulator> >& group = groups[id];

            if (numAccumulators == 0)
                continue; // we are basically building a set

            if (group.empty()) {
                /* add the accumulators */
                group.reserve(numAccumulators);
                for (size_t i = 0; i < numAccumulators; i++) {
                    group.push_back(vpAccumulatorFactory[i]());
                }
            }

            /* tickle all the accumulators for the group we found */
            dassert(numAccumulators == group.size());
            for (size_t i = 0; i < numAccumulators; i++) {
                group[i]->process(vpExpression[i]->evaluate(vars), mergeInputs);
            }
        }

        /* start the group iterator */
        groupsIterator = groups.begin();
        populated = true;
    }

    Document DocumentSourceGroup::makeDocument(const GroupPair& group,
                                               bool mergeableOutput) {
        const Accumulators& accums = group.second;
        const size_t n = vFieldName.size();
        MutableDocument out (1 + n);

        /* add the _id field */
        out.addField("_id", group.first);

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
        intrusive_ptr<ExpressionContext> pMergerExpCtx = pExpCtx->clone();
        pMergerExpCtx->setDoingMerge(true);
        intrusive_ptr<DocumentSourceGroup> pMerger(DocumentSourceGroup::create(pMergerExpCtx));

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
