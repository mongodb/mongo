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
            pCurrent.reset();
            return false;
        }

        pCurrent = makeDocument(groupsIterator);
        return true;
    }

    intrusive_ptr<Document> DocumentSourceGroup::getCurrent() {
        if (!populated)
            populate();

        return pCurrent;
    }

    void DocumentSourceGroup::sourceToBson(BSONObjBuilder *pBuilder) const {
        BSONObjBuilder insides;

        /* add the _id */
        pIdExpression->addToBsonObj(&insides, Document::idName.c_str(), false);

        /* add the remaining fields */
        const size_t n = vFieldName.size();
        for(size_t i = 0; i < n; ++i) {
            intrusive_ptr<Accumulator> pA((*vpAccumulatorFactory[i])(pExpCtx));
            pA->addOperand(vpExpression[i]);
            pA->addToBsonObj(&insides, vFieldName[i], false);
        }

        pBuilder->append(groupName, insides.done());
    }

    intrusive_ptr<DocumentSourceGroup> DocumentSourceGroup::create(
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        intrusive_ptr<DocumentSourceGroup> pSource(
            new DocumentSourceGroup(pExpCtx));
        return pSource;
    }

    DocumentSourceGroup::DocumentSourceGroup(
        const intrusive_ptr<ExpressionContext> &pExpCtx):
        DocumentSource(pExpCtx),
        populated(false),
        pIdExpression(),
        groups(),
        vFieldName(),
        vpAccumulatorFactory(),
        vpExpression() {
    }

    void DocumentSourceGroup::addAccumulator(
        string fieldName,
        intrusive_ptr<Accumulator> (*pAccumulatorFactory)(
            const intrusive_ptr<ExpressionContext> &),
        const intrusive_ptr<Expression> &pExpression) {
        vFieldName.push_back(fieldName);
        vpAccumulatorFactory.push_back(pAccumulatorFactory);
        vpExpression.push_back(pExpression);
    }


    struct GroupOpDesc {
        const char *pName;
        intrusive_ptr<Accumulator> (*pFactory)(
            const intrusive_ptr<ExpressionContext> &);
    };

    static int GroupOpDescCmp(const void *pL, const void *pR) {
        return strcmp(((const GroupOpDesc *)pL)->pName,
                      ((const GroupOpDesc *)pR)->pName);
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

            if (strcmp(pFieldName, Document::idName.c_str()) == 0) {
                uassert(15948, "a group's _id may only be specified once",
                        !idSet);

                BSONType groupType = groupField.type();

                if (groupType == Object) {
                    /*
                      Use the projection-like set of field paths to create the
                      group-by key.
                    */
                    Expression::ObjectCtx oCtx(
                        Expression::ObjectCtx::DOCUMENT_OK);
                    intrusive_ptr<Expression> pId(
                        Expression::parseObject(&groupField, &oCtx));

                    pGroup->setIdExpression(pId);
                    idSet = true;
                }
                else if (groupType == String) {
                    string groupString(groupField.String());
                    const char *pGroupString = groupString.c_str();
                    if ((groupString.length() == 0) ||
                        (pGroupString[0] != '$'))
                        goto StringConstantId;

                    string pathString(
                        Expression::removeFieldPrefix(groupString));
                    intrusive_ptr<ExpressionFieldPath> pFieldPath(
                        ExpressionFieldPath::create(pathString));
                    pGroup->setIdExpression(pFieldPath);
                    idSet = true;
                }
                else {
                    /* pick out the constant types that are allowed */
                    switch(groupType) {
                    case NumberDouble:
                    case String:
                    case Object:
                    case Array:
                    case jstOID:
                    case Bool:
                    case Date:
                    case NumberInt:
                    case Timestamp:
                    case NumberLong:
                    case jstNULL:
                    StringConstantId: // from string case above
                    {
                        intrusive_ptr<const Value> pValue(
                            Value::createFromBsonElement(&groupField));
                        intrusive_ptr<ExpressionConstant> pConstant(
                            ExpressionConstant::create(pValue));
                        pGroup->setIdExpression(pConstant);
                        idSet = true;
                        break;
                    }

                    default:
                        uassert(15949, str::stream() <<
                                "a group's _id may not include fields of BSON type " << groupType,
                                false);
                    }
                }
            }
            else {
                /*
                  Treat as a projection field with the additional ability to
                  add aggregation operators.
                */
                uassert(15950, str::stream() <<
                        "the group aggregate field name \"" <<
                        pFieldName << "\" cannot be an operator name",
                        *pFieldName != '$');

                uassert(15951, str::stream() <<
                        "the group aggregate field \"" << pFieldName <<
                        "\" must be defined as an expression inside an object",
                        groupField.type() == Object);

                BSONObj subField(groupField.Obj());
                BSONObjIterator subIterator(subField);
                size_t subCount = 0;
                for(; subIterator.more(); ++subCount) {
                    BSONElement subElement(subIterator.next());

                    /* look for the specified operator */
                    GroupOpDesc key;
                    key.pName = subElement.fieldName();
                    const GroupOpDesc *pOp =
                        (const GroupOpDesc *)bsearch(
                              &key, GroupOpTable, NGroupOp, sizeof(GroupOpDesc),
                                      GroupOpDescCmp);

                    uassert(15952, str::stream() <<
                            "unknown group operator \"" <<
                            key.pName << "\"",
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
                        uassert(15953, str::stream() <<
                                "aggregating group operators are unary (" <<
                                key.pName << ")", false);
                    }
                    else { /* assume its an atomic single operand */
                        pGroupExpr = Expression::parseOperand(&subElement);
                    }

                    pGroup->addAccumulator(
                        pFieldName, pOp->pFactory, pGroupExpr);
                }

                uassert(15954, str::stream() <<
                        "the computed aggregate \"" <<
                        pFieldName << "\" must specify exactly one operator",
                        subCount == 1);
            }
        }

        uassert(15955, "a group specification must include an _id", idSet);

        return pGroup;
    }

    void DocumentSourceGroup::populate() {
        for(bool hasNext = !pSource->eof(); hasNext;
                hasNext = pSource->advance()) {
            intrusive_ptr<Document> pDocument(pSource->getCurrent());

            /* get the _id document */
            intrusive_ptr<const Value> pId(pIdExpression->evaluate(pDocument));

            /* treat Undefined the same as NULL SERVER-4674 */
            if (pId->getType() == Undefined)
                pId = Value::getNull();

            /*
              Look for the _id value in the map; if it's not there, add a
              new entry with a blank accumulator.
            */
            vector<intrusive_ptr<Accumulator> > *pGroup;
            GroupsType::iterator it(groups.find(pId));
            if (it != groups.end()) {
                /* point at the existing accumulators */
                pGroup = &it->second;
            }
            else {
                /* insert a new group into the map */
                groups.insert(it,
                              pair<intrusive_ptr<const Value>,
                              vector<intrusive_ptr<Accumulator> > >(
                                  pId, vector<intrusive_ptr<Accumulator> >()));

                /* find the accumulator vector (the map value) */
                it = groups.find(pId);
                pGroup = &it->second;

                /* add the accumulators */
                const size_t n = vpAccumulatorFactory.size();
                pGroup->reserve(n);
                for(size_t i = 0; i < n; ++i) {
                    intrusive_ptr<Accumulator> pAccumulator(
                        (*vpAccumulatorFactory[i])(pExpCtx));
                    pAccumulator->addOperand(vpExpression[i]);
                    pGroup->push_back(pAccumulator);
                }
            }

            /* point at the existing key */
            // unneeded atm // pId = it.first;

            /* tickle all the accumulators for the group we found */
            const size_t n = pGroup->size();
            for(size_t i = 0; i < n; ++i)
                (*pGroup)[i]->evaluate(pDocument);
        }

        /* start the group iterator */
        groupsIterator = groups.begin();
        if (groupsIterator != groups.end())
            pCurrent = makeDocument(groupsIterator);
        populated = true;
    }

    intrusive_ptr<Document> DocumentSourceGroup::makeDocument(
        const GroupsType::iterator &rIter) {
        vector<intrusive_ptr<Accumulator> > *pGroup = &rIter->second;
        const size_t n = vFieldName.size();
        intrusive_ptr<Document> pResult(Document::create(1 + n));

        /* add the _id field */
        pResult->addField(Document::idName, rIter->first);

        /* add the rest of the fields */
        for(size_t i = 0; i < n; ++i) {
            intrusive_ptr<const Value> pValue((*pGroup)[i]->getValue());
            if (pValue->getType() != Undefined)
                pResult->addField(vFieldName[i], pValue);
        }

        return pResult;
    }

    intrusive_ptr<DocumentSource> DocumentSourceGroup::createMerger() {
        intrusive_ptr<DocumentSourceGroup> pMerger(
            DocumentSourceGroup::create(pExpCtx));

        /* the merger will use the same grouping key */
        pMerger->setIdExpression(ExpressionFieldPath::create(
                                     Document::idName.c_str()));

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
                ExpressionFieldPath::create(vFieldName[i]));
        }

        return pMerger;
    }
}
