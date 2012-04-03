/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "db/pipeline/expression.h"

#include <cstdio>
#include "db/jsobj.h"
#include "db/pipeline/builder.h"
#include "db/pipeline/dependency_tracker.h"
#include "db/pipeline/document.h"
#include "db/pipeline/expression_context.h"
#include "db/pipeline/value.h"
#include "util/mongoutils/str.h"

namespace mongo {
    using namespace mongoutils;

    /* --------------------------- Expression ------------------------------ */

    void Expression::toMatcherBson(BSONObjBuilder *pBuilder) const {
        verify(false && "Expression::toMatcherBson()");
    }

    Expression::ObjectCtx::ObjectCtx(int theOptions):
        options(theOptions),
        unwindField() {
    }

    void Expression::ObjectCtx::unwind(string fieldName) {
        verify(unwindOk());
        verify(!unwindUsed());
        verify(fieldName.size());
        unwindField = fieldName;
    }

    bool Expression::ObjectCtx::documentOk() const {
        return ((options & DOCUMENT_OK) != 0);
    }

    const char Expression::unwindName[] = "$unwind";

    string Expression::removeFieldPrefix(const string &prefixedField) {
        const char *pPrefixedField = prefixedField.c_str();
        uassert(15982, str::stream() <<
                "field path references must be prefixed with a '$' (\"" <<
                prefixedField << "\"", pPrefixedField[0] == '$');

        return string(pPrefixedField + 1);
    }

    intrusive_ptr<Expression> Expression::parseObject(
        BSONElement *pBsonElement, ObjectCtx *pCtx) {
        /*
          An object expression can take any of the following forms:

          f0: {f1: ..., f2: ..., f3: ...}
          f0: {$operator:[operand1, operand2, ...]}
          f0: {$unwind:"fieldpath"}

          We handle $unwind as a special case, because this is done by the
          projection source.  For any other expression, we hand over control to
          code that parses the expression and returns an expression.
        */

        intrusive_ptr<Expression> pExpression; // the result
        intrusive_ptr<ExpressionObject> pExpressionObject; // alt result
        enum { UNKNOWN, NOTOPERATOR, OPERATOR } kind = UNKNOWN;

        BSONObj obj(pBsonElement->Obj());
        BSONObjIterator iter(obj);
        for(size_t fieldCount = 0; iter.more(); ++fieldCount) {
            BSONElement fieldElement(iter.next());
            const char *pFieldName = fieldElement.fieldName();

            if (pFieldName[0] == '$') {
                uassert(15983, str::stream() <<
                        "the operator must be the only field in a pipeline object (at \""
                        << pFieldName << "\"",
                        fieldCount == 0);

                /* we've determined this "object" is an operator expression */
                kind = OPERATOR;

                pExpression = parseExpression(pFieldName, &fieldElement);
            }
            else {
                uassert(15990, str::stream() << "this object is already an operator expression, and can't be used as a document expression (at \"" <<
                        pFieldName << "\")",
                        kind != OPERATOR);

                /* if it's our first time, create the document expression */
                if (!pExpression.get()) {
                    verify(pCtx->documentOk());
                    // CW TODO error: document not allowed in this context

                    pExpressionObject = ExpressionObject::create();
                    pExpression = pExpressionObject;

                    /* this "object" is not an operator expression */
                    kind = NOTOPERATOR;
                }

                BSONType fieldType = fieldElement.type();
                string fieldName(pFieldName);
                int inclusion = -1;
                if (fieldType == Object) {
                    /* it's a nested document */
                    ObjectCtx oCtx(
                        (pCtx->documentOk() ? ObjectCtx::DOCUMENT_OK : 0));
                    intrusive_ptr<Expression> pNested(
                        parseObject(&fieldElement, &oCtx));
                    pExpressionObject->addField(fieldName, pNested);
                }
                else if (fieldType == String) {
                    /* it's a renamed field */
                    // CW TODO could also be a constant
                    intrusive_ptr<Expression> pPath(
                        ExpressionFieldPath::create(
                            removeFieldPrefix(fieldElement.String())));
                    pExpressionObject->addField(fieldName, pPath);
                }
                else if (fieldType == NumberDouble) {
                    /* it's an inclusion specification */
                    inclusion = static_cast<int>(fieldElement.Double());
                field_inclusion:
                    if (inclusion == 0)
                        pExpressionObject->excludePath(fieldName);
                    else if (inclusion == 1)
                        pExpressionObject->includePath(fieldName);
                    else
                        uassert(15991, str::stream() <<
                                "\"" << fieldName <<
                                "\" numeric inclusion or exclusion must be 1 or 0 (or boolean)",
                                false);
                }
                else if (fieldType == Bool) {
                    inclusion = fieldElement.Bool() ? 1 : 0;
                    goto field_inclusion;
                }
                else if (fieldType == NumberInt) {
                    inclusion = fieldElement.Int();
                    goto field_inclusion;
                }
                else if (fieldType == NumberLong) {
                    inclusion = fieldElement.numberInt();
                    goto field_inclusion;
                }
                else { /* nothing else is allowed */
                    uassert(15992, str::stream() <<
                            "disallowed field type " << fieldType <<
                            " in object expression (at \"" <<
                            fieldName << "\")", false);
                }
            }
        }

        return pExpression;
    }


    struct OpDesc {
        const char *pName;
        intrusive_ptr<ExpressionNary> (*pFactory)(void);

        unsigned flag;
        static const unsigned FIXED_COUNT = 0x0001;
        static const unsigned OBJECT_ARG = 0x0002;

        unsigned argCount;
    };

    static int OpDescCmp(const void *pL, const void *pR) {
        return strcmp(((const OpDesc *)pL)->pName, ((const OpDesc *)pR)->pName);
    }

    /*
      Keep these sorted alphabetically so we can bsearch() them using
      OpDescCmp() above.
    */
    static const OpDesc OpTable[] = {
        {"$add", ExpressionAdd::create, 0},
        {"$and", ExpressionAnd::create, 0},
        {"$cmp", ExpressionCompare::createCmp, OpDesc::FIXED_COUNT, 2},
        {"$cond", ExpressionCond::create, OpDesc::FIXED_COUNT, 3},
        {"$dayOfMonth", ExpressionDayOfMonth::create, OpDesc::FIXED_COUNT, 1},
        {"$dayOfWeek", ExpressionDayOfWeek::create, OpDesc::FIXED_COUNT, 1},
        {"$dayOfYear", ExpressionDayOfYear::create, OpDesc::FIXED_COUNT, 1},
        {"$divide", ExpressionDivide::create, OpDesc::FIXED_COUNT, 2},
        {"$eq", ExpressionCompare::createEq, OpDesc::FIXED_COUNT, 2},
        {"$gt", ExpressionCompare::createGt, OpDesc::FIXED_COUNT, 2},
        {"$gte", ExpressionCompare::createGte, OpDesc::FIXED_COUNT, 2},
        {"$hour", ExpressionHour::create, OpDesc::FIXED_COUNT, 1},
        {"$ifNull", ExpressionIfNull::create, OpDesc::FIXED_COUNT, 2},
        {"$isoDate", ExpressionIsoDate::create,
         OpDesc::FIXED_COUNT | OpDesc::OBJECT_ARG, 1},
        {"$lt", ExpressionCompare::createLt, OpDesc::FIXED_COUNT, 2},
        {"$lte", ExpressionCompare::createLte, OpDesc::FIXED_COUNT, 2},
        {"$minute", ExpressionMinute::create, OpDesc::FIXED_COUNT, 1},
        {"$mod", ExpressionMod::create, OpDesc::FIXED_COUNT, 2},
        {"$month", ExpressionMonth::create, OpDesc::FIXED_COUNT, 1},
        {"$multiply", ExpressionMultiply::create, 0},
        {"$ne", ExpressionCompare::createNe, OpDesc::FIXED_COUNT, 2},
        {"$noOp", ExpressionNoOp::create, OpDesc::FIXED_COUNT, 1},
        {"$not", ExpressionNot::create, OpDesc::FIXED_COUNT, 1},
        {"$or", ExpressionOr::create, 0},
        {"$second", ExpressionSecond::create, OpDesc::FIXED_COUNT, 1},
        {"$strcasecmp", ExpressionStrcasecmp::create, OpDesc::FIXED_COUNT, 2},
        {"$substr", ExpressionSubstr::create, OpDesc::FIXED_COUNT, 3},
        {"$subtract", ExpressionSubtract::create, OpDesc::FIXED_COUNT, 2},
        {"$toLower", ExpressionToLower::create, OpDesc::FIXED_COUNT, 1},
        {"$toUpper", ExpressionToUpper::create, OpDesc::FIXED_COUNT, 1},
        {"$week", ExpressionWeek::create, OpDesc::FIXED_COUNT, 1},
        {"$year", ExpressionYear::create, OpDesc::FIXED_COUNT, 1},
    };

    static const size_t NOp = sizeof(OpTable)/sizeof(OpTable[0]);

    intrusive_ptr<Expression> Expression::parseExpression(
        const char *pOpName, BSONElement *pBsonElement) {
        /* look for the specified operator */
        OpDesc key;
        key.pName = pOpName;
        const OpDesc *pOp = (const OpDesc *)bsearch(
                                &key, OpTable, NOp, sizeof(OpDesc), OpDescCmp);

        uassert(15999, str::stream() << "invalid operator \"" <<
                pOpName << "\"", pOp);

        /* make the expression node */
        intrusive_ptr<ExpressionNary> pExpression((*pOp->pFactory)());

        /* add the operands to the expression node */
        BSONType elementType = pBsonElement->type();

        if (pOp->flag & OpDesc::FIXED_COUNT) {
            if (pOp->argCount > 1)
                uassert(16019, str::stream() << "the " << pOp->pName <<
                        " operator requires an array of " << pOp->argCount <<
                        " operands", elementType == Array);
        }

        if (elementType == Object) {
            /* the operator must be unary and accept an object argument */
            uassert(16021, str::stream() << "the " << pOp->pName <<
                    " operator does not accept an object as an operand",
                    pOp->flag & OpDesc::OBJECT_ARG);

            BSONObj objOperand(pBsonElement->Obj());
            ObjectCtx oCtx(ObjectCtx::DOCUMENT_OK);
            intrusive_ptr<Expression> pOperand(
                Expression::parseObject(pBsonElement, &oCtx));
            pExpression->addOperand(pOperand);
        }
        else if (elementType == Array) {
            /* multiple operands - an n-ary operator */
            vector<BSONElement> bsonArray(pBsonElement->Array());
            const size_t n = bsonArray.size();

            if (pOp->flag & OpDesc::FIXED_COUNT)
                uassert(16020, str::stream() << "the " << pOp->pName <<
                        " operator requires " << pOp->argCount <<
                        " operand(s)", pOp->argCount == n);

            for(size_t i = 0; i < n; ++i) {
                BSONElement *pBsonOperand = &bsonArray[i];
                intrusive_ptr<Expression> pOperand(
                    Expression::parseOperand(pBsonOperand));
                pExpression->addOperand(pOperand);
            }
        }
        else {
            /* assume it's an atomic operand */
            if (pOp->flag & OpDesc::FIXED_COUNT)
                uassert(16022, str::stream() << "the " << pOp->pName <<
                        " operator requires an array of " << pOp->argCount <<
                        " operands", pOp->argCount == 1);

            intrusive_ptr<Expression> pOperand(
                Expression::parseOperand(pBsonElement));
            pExpression->addOperand(pOperand);
        }

        return pExpression;
    }

    intrusive_ptr<Expression> Expression::parseOperand(
        BSONElement *pBsonElement) {
        BSONType type = pBsonElement->type();

        switch(type) {
        case String: {
            /*
              This could be a field path, or it could be a constant
              string.

              We make a copy of the BSONElement reader so we can read its
              value without advancing its state, in case we need to read it
              again in the constant code path.
            */
            BSONElement opCopy(*pBsonElement);
            string value(opCopy.String());

            /* check for a field path */
            if (value[0] != '$')
                goto ExpectConstant;  // assume plain string constant

            /* if we got here, this is a field path expression */
            string fieldPath(removeFieldPrefix(value));
            intrusive_ptr<Expression> pFieldExpr(
                ExpressionFieldPath::create(fieldPath));
            return pFieldExpr;
        }

        case Object: {
            ObjectCtx oCtx(ObjectCtx::DOCUMENT_OK);
            intrusive_ptr<Expression> pSubExpression(
                Expression::parseObject(pBsonElement, &oCtx));
            return pSubExpression;
        }

        default:
        ExpectConstant: {
                intrusive_ptr<Expression> pOperand(
                    ExpressionConstant::createFromBsonElement(pBsonElement));
                return pOperand;
            }

        } // switch(type)

        /* NOTREACHED */
        verify(false);
        return intrusive_ptr<Expression>();
    }

    /* ------------------------- ExpressionAdd ----------------------------- */

    ExpressionAdd::~ExpressionAdd() {
    }

    intrusive_ptr<Expression> ExpressionAdd::optimize() {
        intrusive_ptr<Expression> pE(ExpressionNary::optimize());
        ExpressionAdd *pA = dynamic_cast<ExpressionAdd *>(pE.get());
        if (pA) {
            /* don't create a circular reference */
            if (pA != this)
                pA->pAdd = this;
        }

        return pE;
    }

    intrusive_ptr<ExpressionNary> ExpressionAdd::create() {
        intrusive_ptr<ExpressionAdd> pExpression(new ExpressionAdd());
        return pExpression;
    }

    ExpressionAdd::ExpressionAdd():
        ExpressionNary(),
        useOriginal(false) {
    }

    intrusive_ptr<const Value> ExpressionAdd::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        unsigned stringCount = 0;
        unsigned nonConstStringCount = 0;
        unsigned dateCount = 0;
        const size_t n = vpOperand.size();
        vector<intrusive_ptr<const Value> > vpValue; /* evaluated operands */

        /* use the original, if we've been told to do so */
        if (useOriginal) {
            return pAdd->evaluate(pDocument);
        }

        for (size_t i = 0; i < n; ++i) {
            intrusive_ptr<const Value> pValue(
                vpOperand[i]->evaluate(pDocument));
            vpValue.push_back(pValue);

            BSONType valueType = pValue->getType();
            if (valueType == String) {
                ++stringCount;
                if (!dynamic_cast<ExpressionConstant *>(vpOperand[i].get()))
                    ++nonConstStringCount;
            }
            else if (valueType == Date)
                ++dateCount;
        }

        /* 
           We don't allow adding two dates because it doesn't make sense
           especially since they are in epoch time. However, if there is a
           string present then we would be appending the dates to a string so
           having many would not be not a problem.
        */
        if ((dateCount > 1) && !stringCount) {
            uassert(16000, "can't add two dates together", false);
            return Value::getNull();
        }

        /*
          If there are non-constant strings, and we've got a copy of the
          original, then use that from this point forward.  This is necessary
          to keep the order of strings the same for string concatenation;
          constant-folding would violate the order preservation.

          This is a one-way conversion we do if we see one of these.  It is
          possible that these could vary from document to document, but any
          sane schema probably isn't going to do that, so once we see a string,
          we can probably assume they're going to be strings all the way down.
         */
        if (nonConstStringCount && pAdd.get()) {
            useOriginal = true;
            return pAdd->evaluate(pDocument);
        }

        if (stringCount) {
            stringstream stringTotal;
            for (size_t i = 0; i < n; ++i) {
                intrusive_ptr<const Value> pValue(vpValue[i]);
                stringTotal << pValue->coerceToString();
            }

            return Value::createString(stringTotal.str());
        }

        if (dateCount) {
            long long dateTotal = 0;
            for (size_t i = 0; i < n; ++i) {
                intrusive_ptr<const Value> pValue(vpValue[i]);
                if (pValue->getType() == Date) 
                    dateTotal += pValue->coerceToDate();
                else 
                    dateTotal += static_cast<long long>(pValue->coerceToDouble()*24*60*60*1000);
            }

            return Value::createDate(Date_t(dateTotal));
        }

        /*
          We'll try to return the narrowest possible result value.  To do that
          without creating intermediate Values, do the arithmetic for double
          and integral types in parallel, tracking the current narrowest
          type.
         */
        double doubleTotal = 0;
        long long longTotal = 0;
        BSONType totalType = NumberInt;
        for(size_t i = 0; i < n; ++i) {
            intrusive_ptr<const Value> pValue(vpValue[i]);

            totalType = Value::getWidestNumeric(totalType, pValue->getType());
            doubleTotal += pValue->coerceToDouble();
            longTotal += pValue->coerceToLong();
        }

        if (totalType == NumberDouble)
            return Value::createDouble(doubleTotal);
        if (totalType == NumberLong)
            return Value::createLong(longTotal);
        return Value::createInt((int)longTotal);
    }

    const char *ExpressionAdd::getOpName() const {
        return "$add";
    }

    intrusive_ptr<ExpressionNary> (*ExpressionAdd::getFactory() const)() {
        return ExpressionAdd::create;
    }

    void ExpressionAdd::toBson(
        BSONObjBuilder *pBuilder, const char *pOpName) const {

        if (pAdd)
            pAdd->toBson(pBuilder, pOpName);
        else
            ExpressionNary::toBson(pBuilder, pOpName);
    }


    /* ------------------------- ExpressionAnd ----------------------------- */

    ExpressionAnd::~ExpressionAnd() {
    }

    intrusive_ptr<ExpressionNary> ExpressionAnd::create() {
        intrusive_ptr<ExpressionNary> pExpression(new ExpressionAnd());
        return pExpression;
    }

    ExpressionAnd::ExpressionAnd():
        ExpressionNary() {
    }

    intrusive_ptr<Expression> ExpressionAnd::optimize() {
        /* optimize the conjunction as much as possible */
        intrusive_ptr<Expression> pE(ExpressionNary::optimize());

        /* if the result isn't a conjunction, we can't do anything */
        ExpressionAnd *pAnd = dynamic_cast<ExpressionAnd *>(pE.get());
        if (!pAnd)
            return pE;

        /*
          Check the last argument on the result; if it's not constant (as
          promised by ExpressionNary::optimize(),) then there's nothing
          we can do.
        */
        const size_t n = pAnd->vpOperand.size();
        intrusive_ptr<Expression> pLast(pAnd->vpOperand[n - 1]);
        const ExpressionConstant *pConst =
            dynamic_cast<ExpressionConstant *>(pLast.get());
        if (!pConst)
            return pE;

        /*
          Evaluate and coerce the last argument to a boolean.  If it's false,
          then we can replace this entire expression.
         */
        bool last = pLast->evaluate(intrusive_ptr<Document>())->coerceToBool();
        if (!last) {
            intrusive_ptr<ExpressionConstant> pFinal(
                ExpressionConstant::create(Value::getFalse()));
            return pFinal;
        }

        /*
          If we got here, the final operand was true, so we don't need it
          anymore.  If there was only one other operand, we don't need the
          conjunction either.  Note we still need to keep the promise that
          the result will be a boolean.
         */
        if (n == 2) {
            intrusive_ptr<Expression> pFinal(
                ExpressionCoerceToBool::create(pAnd->vpOperand[0]));
            return pFinal;
        }

        /*
          Remove the final "true" value, and return the new expression.

          CW TODO:
          Note that because of any implicit conversions, we may need to
          apply an implicit boolean conversion.
        */
        pAnd->vpOperand.resize(n - 1);
        return pE;
    }

    intrusive_ptr<const Value> ExpressionAnd::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        const size_t n = vpOperand.size();
        for(size_t i = 0; i < n; ++i) {
            intrusive_ptr<const Value> pValue(vpOperand[i]->evaluate(pDocument));
            if (!pValue->coerceToBool())
                return Value::getFalse();
        }

        return Value::getTrue();
    }

    const char *ExpressionAnd::getOpName() const {
        return "$and";
    }

    void ExpressionAnd::toMatcherBson(BSONObjBuilder *pBuilder) const {
        /*
          There are two patterns we can handle:
          (1) one or two comparisons on the same field: { a:{$gte:3, $lt:7} }
          (2) multiple field comparisons: {a:7, b:{$lte:6}, c:2}
            This can be recognized as a conjunction of a set of  range
            expressions.  Direct equality is a degenerate range expression;
            range expressions can be open-ended.
        */
        verify(false && "unimplemented");
    }

    intrusive_ptr<ExpressionNary> (*ExpressionAnd::getFactory() const)() {
        return ExpressionAnd::create;
    }

    /* -------------------- ExpressionCoerceToBool ------------------------- */

    ExpressionCoerceToBool::~ExpressionCoerceToBool() {
    }

    intrusive_ptr<ExpressionCoerceToBool> ExpressionCoerceToBool::create(
        const intrusive_ptr<Expression> &pExpression) {
        intrusive_ptr<ExpressionCoerceToBool> pNew(
            new ExpressionCoerceToBool(pExpression));
        return pNew;
    }

    ExpressionCoerceToBool::ExpressionCoerceToBool(
        const intrusive_ptr<Expression> &pTheExpression):
        Expression(),
        pExpression(pTheExpression) {
    }

    intrusive_ptr<Expression> ExpressionCoerceToBool::optimize() {
        /* optimize the operand */
        pExpression = pExpression->optimize();

        /* if the operand already produces a boolean, then we don't need this */
        /* LATER - Expression to support a "typeof" query? */
        Expression *pE = pExpression.get();
        if (dynamic_cast<ExpressionAnd *>(pE) ||
            dynamic_cast<ExpressionOr *>(pE) ||
            dynamic_cast<ExpressionNot *>(pE) ||
            dynamic_cast<ExpressionCoerceToBool *>(pE))
            return pExpression;

        return intrusive_ptr<Expression>(this);
    }

    void ExpressionCoerceToBool::addDependencies(
        const intrusive_ptr<DependencyTracker> &pTracker,
        const DocumentSource *pSource) const {
        /* nothing to do */
    }

    intrusive_ptr<const Value> ExpressionCoerceToBool::evaluate(
        const intrusive_ptr<Document> &pDocument) const {

        intrusive_ptr<const Value> pResult(pExpression->evaluate(pDocument));
        bool b = pResult->coerceToBool();
        if (b)
            return Value::getTrue();
        return Value::getFalse();
    }

    void ExpressionCoerceToBool::addToBsonObj(
        BSONObjBuilder *pBuilder, string fieldName,
        bool requireExpression) const {
        verify(false && "not possible"); // no equivalent of this
    }

    void ExpressionCoerceToBool::addToBsonArray(
        BSONArrayBuilder *pBuilder) const {
        verify(false && "not possible"); // no equivalent of this
    }

    /* ----------------------- ExpressionCompare --------------------------- */

    ExpressionCompare::~ExpressionCompare() {
    }

    intrusive_ptr<ExpressionNary> ExpressionCompare::createEq() {
        intrusive_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(EQ));
        return pExpression;
    }

    intrusive_ptr<ExpressionNary> ExpressionCompare::createNe() {
        intrusive_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(NE));
        return pExpression;
    }

    intrusive_ptr<ExpressionNary> ExpressionCompare::createGt() {
        intrusive_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(GT));
        return pExpression;
    }

    intrusive_ptr<ExpressionNary> ExpressionCompare::createGte() {
        intrusive_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(GTE));
        return pExpression;
    }

    intrusive_ptr<ExpressionNary> ExpressionCompare::createLt() {
        intrusive_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(LT));
        return pExpression;
    }

    intrusive_ptr<ExpressionNary> ExpressionCompare::createLte() {
        intrusive_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(LTE));
        return pExpression;
    }

    intrusive_ptr<ExpressionNary> ExpressionCompare::createCmp() {
        intrusive_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(CMP));
        return pExpression;
    }

    ExpressionCompare::ExpressionCompare(CmpOp theCmpOp):
        ExpressionNary(),
        cmpOp(theCmpOp) {
    }

    void ExpressionCompare::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(2);
        ExpressionNary::addOperand(pExpression);
    }

    /*
      Lookup table for truth value returns
    */
    struct CmpLookup {
        bool truthValue[3]; /* truth value for -1, 0, 1 */
        Expression::CmpOp reverse; /* reverse comparison operator */
        char name[5]; /* string name (w/trailing '\0') */
    };
    static const CmpLookup cmpLookup[7] = {
        /*             -1      0      1      reverse          name   */
        /* EQ  */ { { false, true,  false }, Expression::EQ,  "$eq"  },
        /* NE  */ { { true,  false, true },  Expression::NE,  "$ne"  },
        /* GT  */ { { false, false, true },  Expression::LTE, "$gt"  },
        /* GTE */ { { false, true,  true },  Expression::LT,  "$gte" },
        /* LT  */ { { true,  false, false }, Expression::GTE, "$lt"  },
        /* LTE */ { { true,  true,  false }, Expression::GT,  "$lte" },
        /* CMP */ { { false, false, false }, Expression::CMP, "$cmp" },
    };

    intrusive_ptr<Expression> ExpressionCompare::optimize() {
        /* first optimize the comparison operands */
        intrusive_ptr<Expression> pE(ExpressionNary::optimize());

        /*
          If the result of optimization is no longer a comparison, there's
          nothing more we can do.
        */
        ExpressionCompare *pCmp = dynamic_cast<ExpressionCompare *>(pE.get());
        if (!pCmp)
            return pE;

        /* check to see if optimizing comparison operator is supported */
        CmpOp newOp = pCmp->cmpOp;
        if (newOp == CMP)
            return pE; // not reversible: there's nothing more we can do

        /*
          There's one localized optimization we recognize:  a comparison
          between a field and a constant.  If we recognize that pattern,
          replace it with an ExpressionFieldRange.

          When looking for this pattern, note that the operands could appear
          in any order.  If we need to reverse the sense of the comparison to
          put it into the required canonical form, do so.
         */
        intrusive_ptr<Expression> pLeft(pCmp->vpOperand[0]);
        intrusive_ptr<Expression> pRight(pCmp->vpOperand[1]);
        intrusive_ptr<ExpressionFieldPath> pFieldPath(
            dynamic_pointer_cast<ExpressionFieldPath>(pLeft));
        intrusive_ptr<ExpressionConstant> pConstant;
        if (pFieldPath.get()) {
            pConstant = dynamic_pointer_cast<ExpressionConstant>(pRight);
            if (!pConstant.get())
                return pE; // there's nothing more we can do
        }
        else {
            /* if the first operand wasn't a path, see if it's a constant */
            pConstant = dynamic_pointer_cast<ExpressionConstant>(pLeft);
            if (!pConstant.get())
                return pE; // there's nothing more we can do

            /* the left operand was a constant; see if the right is a path */
            pFieldPath = dynamic_pointer_cast<ExpressionFieldPath>(pRight);
            if (!pFieldPath.get())
                return pE; // there's nothing more we can do

            /* these were not in canonical order, so reverse the sense */
            newOp = cmpLookup[newOp].reverse;
        }

        return ExpressionFieldRange::create(
            pFieldPath, newOp, pConstant->getValue());
    }

    intrusive_ptr<const Value> ExpressionCompare::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(2);
        intrusive_ptr<const Value> pLeft(vpOperand[0]->evaluate(pDocument));
        intrusive_ptr<const Value> pRight(vpOperand[1]->evaluate(pDocument));

        /* TODO look into collapsing by using Value::compare() */

        BSONType leftType = pLeft->getType();
        BSONType rightType = pRight->getType();
        uassert(15994, str::stream() << getOpName() <<
                ":  no automatic conversion for types " <<
                leftType << " and " << rightType,
                leftType == rightType);
        // CW TODO at least for now.  later, handle automatic conversions

        int cmp = 0;
        switch(leftType) {
        case NumberDouble: {
            double left = pLeft->getDouble();
            double right = pRight->getDouble();

            if (left < right)
                cmp = -1;
            else if (left > right)
                cmp = 1;
            break;
        }

        case NumberInt: {
            int left = pLeft->getInt();
            int right = pRight->getInt();

            if (left < right)
                cmp = -1;
            else if (left > right)
                cmp = 1;
            break;
        }

        case String: {
            string left(pLeft->getString());
            string right(pRight->getString());
            cmp = signum(left.compare(right));
            break;
        }

        case Date:
            cmp = signum(Value::compare(pLeft, pRight));
            break;

        default:
            uassert(15995, str::stream() <<
                    "can't compare values of type " << leftType, false);
            break;
        }

        if (cmpOp == CMP) {
            switch(cmp) {
            case -1:
                return Value::getMinusOne();
            case 0:
                return Value::getZero();
            case 1:
                return Value::getOne();

            default:
                verify(false); // CW TODO internal error
                return Value::getNull();
            }
        }

        bool returnValue = cmpLookup[cmpOp].truthValue[cmp + 1];
        if (returnValue)
            return Value::getTrue();
        return Value::getFalse();
    }

    const char *ExpressionCompare::getOpName() const {
        return cmpLookup[cmpOp].name;
    }

    /* ----------------------- ExpressionCond ------------------------------ */

    ExpressionCond::~ExpressionCond() {
    }

    intrusive_ptr<ExpressionNary> ExpressionCond::create() {
        intrusive_ptr<ExpressionCond> pExpression(new ExpressionCond());
        return pExpression;
    }

    ExpressionCond::ExpressionCond():
        ExpressionNary() {
    }

    void ExpressionCond::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(3);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionCond::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(3);
        intrusive_ptr<const Value> pCond(vpOperand[0]->evaluate(pDocument));
        int idx = pCond->coerceToBool() ? 1 : 2;
        return vpOperand[idx]->evaluate(pDocument);
    }

    const char *ExpressionCond::getOpName() const {
        return "$cond";
    }

    /* ---------------------- ExpressionConstant --------------------------- */

    ExpressionConstant::~ExpressionConstant() {
    }

    intrusive_ptr<ExpressionConstant> ExpressionConstant::createFromBsonElement(
        BSONElement *pBsonElement) {
        intrusive_ptr<ExpressionConstant> pEC(
            new ExpressionConstant(pBsonElement));
        return pEC;
    }

    ExpressionConstant::ExpressionConstant(BSONElement *pBsonElement):
        pValue(Value::createFromBsonElement(pBsonElement)) {
    }

    intrusive_ptr<ExpressionConstant> ExpressionConstant::create(
        const intrusive_ptr<const Value> &pValue) {
        intrusive_ptr<ExpressionConstant> pEC(new ExpressionConstant(pValue));
        return pEC;
    }

    ExpressionConstant::ExpressionConstant(
        const intrusive_ptr<const Value> &pTheValue):
        pValue(pTheValue) {
    }


    intrusive_ptr<Expression> ExpressionConstant::optimize() {
        /* nothing to do */
        return intrusive_ptr<Expression>(this);
    }

    void ExpressionConstant::addDependencies(
        const intrusive_ptr<DependencyTracker> &pTracker,
        const DocumentSource *pSource) const {
        /* nothing to do */
    }

    intrusive_ptr<const Value> ExpressionConstant::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        return pValue;
    }

    void ExpressionConstant::addToBsonObj(
        BSONObjBuilder *pBuilder, string fieldName,
        bool requireExpression) const {
        /*
          If we don't need an expression, but can use a naked scalar,
          do the regular thing.

          This is geared to handle $project, which uses expressions as a cue
          that the field is a new virtual field rather than just an
          inclusion (or exclusion):
          { $project : {
              x : true, // include
              y : { $const: true }
          }}

          This can happen as a result of optimizations.  For example, the
          above may have originally been
          { $project : {
              x : true, // include
              y : { $eq:["foo", "foo"] }
          }}
          When this is optimized, the $eq will be replaced with true.  However,
          if the pipeline is rematerialized (as happens for a split for
          sharding) and sent to another node, it will now have
              y : true
          which will look like an inclusion rather than a computed field.
        */
        if (!requireExpression) {
            pValue->addToBsonObj(pBuilder, fieldName);
            return;
        }

        /*
          We require an expression, so build one here, and use that.

          Note we emit a $noOp expression.  This is because the table-driven
          expression parser requires an ExpressionNary factory, and
          ExpressionConstant isn't an ExpressionNary.  However, the generated
          NoOp will go away in its ::optimize() phase.
        */
        BSONObjBuilder constBuilder;
        pValue->addToBsonObj(&constBuilder, "$noOp");
        pBuilder->append(fieldName, constBuilder.done());
    }

    void ExpressionConstant::addToBsonArray(
        BSONArrayBuilder *pBuilder) const {
        pValue->addToBsonArray(pBuilder);
    }

    const char *ExpressionConstant::getOpName() const {
        return "$const";
    }

    /* ---------------------- ExpressionDayOfMonth ------------------------- */

    ExpressionDayOfMonth::~ExpressionDayOfMonth() {
    }

    intrusive_ptr<ExpressionNary> ExpressionDayOfMonth::create() {
        intrusive_ptr<ExpressionDayOfMonth> pExpression(new ExpressionDayOfMonth());
        return pExpression;
    }

    ExpressionDayOfMonth::ExpressionDayOfMonth():
        ExpressionNary() {
    }

    void ExpressionDayOfMonth::addOperand(const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);

        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionDayOfMonth::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(1);
        intrusive_ptr<const Value> pDate(vpOperand[0]->evaluate(pDocument));
        tm date;
        (pDate->coerceToDate()).toTm(&date);
        return Value::createInt(date.tm_mday); 
    }

    const char *ExpressionDayOfMonth::getOpName() const {
        return "$dayOfMonth";
    }

    /* ------------------------- ExpressionDayOfWeek ----------------------------- */

    ExpressionDayOfWeek::~ExpressionDayOfWeek() {
    }

    intrusive_ptr<ExpressionNary> ExpressionDayOfWeek::create() {
        intrusive_ptr<ExpressionDayOfWeek> pExpression(new ExpressionDayOfWeek());
        return pExpression;
    }

    ExpressionDayOfWeek::ExpressionDayOfWeek():
        ExpressionNary() {
    }

    void ExpressionDayOfWeek::addOperand(const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionDayOfWeek::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(1);
        intrusive_ptr<const Value> pDate(vpOperand[0]->evaluate(pDocument));
        tm date;
        (pDate->coerceToDate()).toTm(&date);
        return Value::createInt(date.tm_wday+1); // MySQL uses 1-7 tm uses 0-6
    }

    const char *ExpressionDayOfWeek::getOpName() const {
        return "$dayOfWeek";
    }

    /* ------------------------- ExpressionDayOfYear ----------------------------- */

    ExpressionDayOfYear::~ExpressionDayOfYear() {
    }

    intrusive_ptr<ExpressionNary> ExpressionDayOfYear::create() {
        intrusive_ptr<ExpressionDayOfYear> pExpression(new ExpressionDayOfYear());
        return pExpression;
    }

    ExpressionDayOfYear::ExpressionDayOfYear():
        ExpressionNary() {
    }

    void ExpressionDayOfYear::addOperand(const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionDayOfYear::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(1);
        intrusive_ptr<const Value> pDate(vpOperand[0]->evaluate(pDocument));
        tm date;
        (pDate->coerceToDate()).toTm(&date);
        return Value::createInt(date.tm_yday+1); // MySQL uses 1-366 tm uses 0-365
    }

    const char *ExpressionDayOfYear::getOpName() const {
        return "$dayOfYear";
    }

    /* ----------------------- ExpressionDivide ---------------------------- */

    ExpressionDivide::~ExpressionDivide() {
    }

    intrusive_ptr<ExpressionNary> ExpressionDivide::create() {
        intrusive_ptr<ExpressionDivide> pExpression(new ExpressionDivide());
        return pExpression;
    }

    ExpressionDivide::ExpressionDivide():
        ExpressionNary() {
    }

    void ExpressionDivide::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(2);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionDivide::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(2);
        intrusive_ptr<const Value> pLeft(vpOperand[0]->evaluate(pDocument));
        intrusive_ptr<const Value> pRight(vpOperand[1]->evaluate(pDocument));

        double right = pRight->coerceToDouble();
        if (right == 0)
            return Value::getUndefined();

        double left = pLeft->coerceToDouble();

        return Value::createDouble(left / right);
    }

    const char *ExpressionDivide::getOpName() const {
        return "$divide";
    }

    /* ---------------------- ExpressionObject --------------------------- */

    ExpressionObject::~ExpressionObject() {
    }

    intrusive_ptr<ExpressionObject> ExpressionObject::create() {
        intrusive_ptr<ExpressionObject> pExpression(new ExpressionObject());
        return pExpression;
    }

    ExpressionObject::ExpressionObject():
        excludePaths(false),
        path(),
        vFieldName(),
        vpExpression() {
    }

    intrusive_ptr<Expression> ExpressionObject::optimize() {
        const size_t n = vpExpression.size();
        for(size_t i = 0; i < n; ++i) {
            intrusive_ptr<Expression> pE(vpExpression[i]->optimize());
            vpExpression[i] = pE;
        }

        return intrusive_ptr<Expression>(this);
    }

    void ExpressionObject::addDependencies(
        const intrusive_ptr<DependencyTracker> &pTracker,
        const DocumentSource *pSource) const {
        for(ExpressionVector::const_iterator i(vpExpression.begin());
            i != vpExpression.end(); ++i) {
            (*i)->addDependencies(pTracker, pSource);
        }
    }

    void ExpressionObject::addToDocument(
        const intrusive_ptr<Document> &pResult,
        const intrusive_ptr<Document> &pDocument,
        bool excludeId) const {
        const size_t pathSize = path.size();
        set<string>::const_iterator end(path.end());

        if (pathSize) {
            auto_ptr<FieldIterator> pIter(pDocument->createFieldIterator());
            if (excludePaths) {
                while(pIter->more()) {
                    pair<string, intrusive_ptr<const Value> > field(pIter->next());

                    /*
                      If we're excluding _id, and this is it, skip it.

                      If the field in the document is not in the exclusion set,
                      add it to the result document.

                      Note that exclusions are only allowed on leaves, so we
                      can assume we don't have to descend recursively here.
                     */
                    if ((excludeId && !field.first.compare(Document::idName)) ||
                        (path.find(field.first) != end))
                        continue; // we found it, so don't add it

                    pResult->addField(field.first, field.second);
                }
            }
            else { /* !excludePaths */
                while(pIter->more()) {
                    pair<string, intrusive_ptr<const Value> > field(
                        pIter->next());
                    /*
                      Note that this could be an inclusion along a pathway,
                      so we look for an ExpressionObject in vpExpression; when
                      we find one, we populate the result with the evaluation
                      of that on the nested object, yielding relative paths.
                      This also allows us to handle intermediate arrays; if we
                      encounter one, we repeat this for each array element.
                     */
                    if (path.find(field.first) != end) {
                        /* find the Expression */
                        const size_t n = vFieldName.size();
                        size_t i;
                        Expression *pE = NULL;
                        for(i = 0; i < n; ++i) {
                            if (field.first.compare(vFieldName[i]) == 0) {
                                pE = vpExpression[i].get();
                                break;
                            }
                        }

                        /*
                          If we didn't find an expression, it's the last path
                          element to include.
                        */
                        if (!pE) {
                            pResult->addField(field.first, field.second);
                            continue;
                        }

                        ExpressionObject *pChild =
                            dynamic_cast<ExpressionObject *>(pE);
                        verify(pChild);

                        /*
                          Check on the type of the result object.  If it's an
                          object, just walk down into that recursively, and
                          add it to the result.
                        */
                        BSONType valueType = field.second->getType();
                        if (valueType == Object) {
                            intrusive_ptr<Document> pD(
                                pChild->evaluateDocument(
                                    field.second->getDocument()));
                            pResult->addField(vFieldName[i],
                                              Value::createDocument(pD));
                        }
                        else if (valueType == Array) {
                            /*
                              If it's an array, we have to do the same thing,
                              but to each array element.  Then, add the array
                              of results to the current document.
                            */
                            vector<intrusive_ptr<const Value> > result;
                            intrusive_ptr<ValueIterator> pVI(
                                field.second->getArray());
                            while(pVI->more()) {
                                intrusive_ptr<Document> pD(
                                    pChild->evaluateDocument(
                                        pVI->next()->getDocument()));
                                result.push_back(Value::createDocument(pD));
                            }

                            pResult->addField(vFieldName[i],
                                              Value::createArray(result));
                        }
                    }
                }
            }
        }

        /* add any remaining fields we haven't already taken care of */
        const size_t n = vFieldName.size();
        for(size_t i = 0; i < n; ++i) {
            string fieldName(vFieldName[i]);

            /* if we've already dealt with this field, above, do nothing */
            if (path.find(fieldName) != end)
                continue;

            intrusive_ptr<const Value> pValue(
                vpExpression[i]->evaluate(pDocument));

            /*
              Don't add non-existent values (note:  different from NULL);
              this is consistent with existing selection syntax which doesn't
              force the appearnance of non-existent fields.
            */
            if (pValue->getType() == Undefined)
                continue;

            pResult->addField(fieldName, pValue);
        }
    }

    size_t ExpressionObject::getSizeHint(
        const intrusive_ptr<Document> &pDocument) const {
        size_t sizeHint = pDocument->getFieldCount();
        const size_t pathSize = path.size();
        if (!excludePaths)
            sizeHint += pathSize;
        else {
            size_t excludeCount = pathSize;
            if (sizeHint > excludeCount)
                sizeHint -= excludeCount;
            else
                sizeHint = 0;
        }

        /* account for the additional computed fields */
        sizeHint += vFieldName.size();

        return sizeHint;
    }

    intrusive_ptr<Document> ExpressionObject::evaluateDocument(
        const intrusive_ptr<Document> &pDocument) const {
        /* create and populate the result */
        intrusive_ptr<Document> pResult(
            Document::create(getSizeHint(pDocument)));
        addToDocument(pResult, pDocument);
        return pResult;
    }

    intrusive_ptr<const Value> ExpressionObject::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        return Value::createDocument(evaluateDocument(pDocument));
    }

    void ExpressionObject::addField(const string &fieldName,
                                    const intrusive_ptr<Expression> &pExpression) {
        /* must have an expression */
        verify(pExpression.get());

        /* parse the field path */
        FieldPath fieldPath(fieldName);
        uassert(16008, str::stream() <<
                "an expression object's field names cannot be field paths (at \"" <<
                fieldName << "\")", fieldPath.getPathLength() == 1);

        /* make sure it isn't a name we've included or excluded */
        set<string>::iterator ex(path.find(fieldName));
        uassert(16009, str::stream() <<
                "can't add a field to an object expression that has already been excluded (at \"" <<
                fieldName << "\")", ex == path.end());

        /* make sure it isn't a name we've already got */
        const size_t n = vFieldName.size();
        for(size_t i = 0; i < n; ++i) {
            uassert(16010, str::stream() <<
                    "can't add the same field to an object expression more than once (at \"" <<
                    fieldName << "\")",
                    fieldName.compare(vFieldName[i]) != 0);
        }

        vFieldName.push_back(fieldName);
        vpExpression.push_back(pExpression);
    }

    void ExpressionObject::includePath(
        const FieldPath *pPath, size_t pathi, size_t pathn, bool excludeLast) {

        /* get the current path field name */
        string fieldName(pPath->getFieldName(pathi));
        uassert(16011,
                "an object expression can't include an empty field-name",
                fieldName.length());

        const size_t pathCount = path.size();

        /* if this is the leaf-most object, stop */
        if (pathi == pathn - 1) {
            /*
              Make sure the exclusion configuration of this node matches
              the requested result.  Or, that this is the first (determining)
              specification.
            */
            uassert(16012, str::stream() <<
                    "incompatible exclusion for \"" <<
                    pPath->getPath(false) <<
                    "\" because of a prior inclusion that includes a common sub-path",
                    ((excludePaths == excludeLast) || !pathCount));

            excludePaths = excludeLast; // if (!pathCount), set this
            path.insert(fieldName);
            return;
        }

        /* this level had better be about inclusions */
        uassert(16013, str::stream() <<
                "incompatible inclusion for \"" << pPath->getPath(false) <<
                "\" because of a prior exclusion that includes a common sub-path",
                !excludePaths);

        /* see if we already know about this field */
        const size_t n = vFieldName.size();
        size_t i;
        for(i = 0; i < n; ++i) {
            if (fieldName.compare(vFieldName[i]) == 0)
                break;
        }

        /* find the right object, and continue */
        ExpressionObject *pChild;
        if (i < n) {
            /* the intermediate child already exists */
            pChild = dynamic_cast<ExpressionObject *>(vpExpression[i].get());
            verify(pChild);
        }
        else {
            /*
              If we get here, the intervening child isn't already there,
              so create it.
            */
            intrusive_ptr<ExpressionObject> pSharedChild(
                ExpressionObject::create());
            path.insert(fieldName);
            vFieldName.push_back(fieldName);
            vpExpression.push_back(pSharedChild);
            pChild = pSharedChild.get();
        }

        // LATER CW TODO turn this into a loop
        pChild->includePath(pPath, pathi + 1, pathn, excludeLast);
    }

    void ExpressionObject::includePath(const string &theFieldPath) {
        /* parse the field path */
        FieldPath fieldPath(theFieldPath);
        includePath(&fieldPath, 0, fieldPath.getPathLength(), false);
    }

    void ExpressionObject::excludePath(const string &theFieldPath) {
        /* parse the field path */
        FieldPath fieldPath(theFieldPath);
        includePath(&fieldPath, 0, fieldPath.getPathLength(), true);
    }

    Iterator<string> *ExpressionObject::getFieldIterator() const {
        intrusive_ptr<const ExpressionObject> pThis(this);
        return new IteratorVectorIntrusive<string, ExpressionObject>(
            vFieldName, pThis);
    }

    intrusive_ptr<Expression> ExpressionObject::getField(
        const string &fieldName) const {
        const size_t n = vFieldName.size();
        for(size_t i = 0; i < n; ++i) {
            if (fieldName.compare(vFieldName[i]) == 0)
                return vpExpression[i];
        }

        /* if we got here, we didn't find it */
        return intrusive_ptr<Expression>();
    }

    void ExpressionObject::emitPaths(PathSink *pPathSink) const {
        vector<string> vPath;
        emitPaths(pPathSink, &vPath);
    }

    void ExpressionObject::emitPaths(
        PathSink *pPathSink, vector<string> *pvPath) const {
        if (!path.size())
            return;
        
        /* we use these for loops */
        const size_t nField = vFieldName.size();
        const size_t nPath = pvPath->size();

        /*
          We can iterate over the inclusion/exclusion paths in their
          (random) set order because they don't affect the order that
          fields are listed in the result.  That comes from the underlying
          Document they are fetched from.
         */
        for(set<string>::const_iterator end(path.end()),
                iter(path.begin()); iter != end; ++iter) {

            /* find the matching field description */
            size_t iField = 0;
            for(; iField < nField; ++iField) {
                if (iter->compare(vFieldName[iField]) == 0)
                    break;
            }

            if (iField == nField) {
                /*
                  If we didn't find a matching field description, this is the
                  leaf, so add the path.
                */
                stringstream ss;

                for(size_t iPath = 0; iPath < nPath; ++iPath)
                    ss << (*pvPath)[iPath] << ".";
                ss << *iter;

                pPathSink->path(ss.str(), !excludePaths);
            }
            else {
                /*
                  If we found a matching field description, then we need to
                  descend into the next level.
                */
                Expression *pE = vpExpression[iField].get();
                ExpressionObject *pEO = dynamic_cast<ExpressionObject *>(pE);
                verify(pEO);

                /*
                  Add the current field name to the path being built up,
                  then go down into the next level.
                 */
                PathPusher pathPusher(pvPath, vFieldName[iField]);
                pEO->emitPaths(pPathSink, pvPath);
            }
        }
    }

    void ExpressionObject::documentToBson(
        BSONObjBuilder *pBuilder, bool requireExpression) const {

        /* emit any inclusion/exclusion paths */
        BuilderPathSink builderPathSink(pBuilder);
        emitPaths(&builderPathSink);

        /* then add any expressions */
        const size_t nField = vFieldName.size();
        const set<string>::const_iterator pathEnd(path.end());
        for(size_t iField = 0; iField < nField; ++iField) {
            string fieldName(vFieldName[iField]);

            /* if we already took care of this, don't repeat it */
            if (path.find(fieldName) != pathEnd)
                continue;

            vpExpression[iField]->addToBsonObj(
                pBuilder, fieldName, requireExpression);
        }
    }

    void ExpressionObject::addToBsonObj(
        BSONObjBuilder *pBuilder, string fieldName,
        bool requireExpression) const {

        BSONObjBuilder objBuilder;
        documentToBson(&objBuilder, requireExpression);
        pBuilder->append(fieldName, objBuilder.done());
    }

    void ExpressionObject::addToBsonArray(
        BSONArrayBuilder *pBuilder) const {

        BSONObjBuilder objBuilder;
        documentToBson(&objBuilder, false);
        pBuilder->append(objBuilder.done());
    }

    void ExpressionObject::BuilderPathSink::path(
        const string &path, bool include) {
        pBuilder->append(path, include);
    }

    /* --------------------- ExpressionFieldPath --------------------------- */

    ExpressionFieldPath::~ExpressionFieldPath() {
    }

    intrusive_ptr<ExpressionFieldPath> ExpressionFieldPath::create(
        const string &fieldPath) {
        intrusive_ptr<ExpressionFieldPath> pExpression(
            new ExpressionFieldPath(fieldPath));
        return pExpression;
    }

    ExpressionFieldPath::ExpressionFieldPath(
        const string &theFieldPath):
        fieldPath(theFieldPath) {
    }

    intrusive_ptr<Expression> ExpressionFieldPath::optimize() {
        /* nothing can be done for these */
        return intrusive_ptr<Expression>(this);
    }

    void ExpressionFieldPath::addDependencies(
        const intrusive_ptr<DependencyTracker> &pTracker,
        const DocumentSource *pSource) const {
        pTracker->addDependency(fieldPath.getPath(false), pSource);
    }

    intrusive_ptr<const Value> ExpressionFieldPath::evaluatePath(
        size_t index, const size_t pathLength,
        intrusive_ptr<Document> pDocument) const {
        intrusive_ptr<const Value> pValue; /* the return value */

        pValue = pDocument->getValue(fieldPath.getFieldName(index));

        /* if the field doesn't exist, quit with an undefined value */
        if (!pValue.get())
            return Value::getUndefined();

        /* if we've hit the end of the path, stop */
        ++index;
        if (index >= pathLength)
            return pValue;

        /*
          We're diving deeper.  If the value was null, return null.
        */
        BSONType type = pValue->getType();
        if ((type == Undefined) || (type == jstNULL))
            return Value::getUndefined();

        if (type == Object) {
            /* extract from the next level down */
            return evaluatePath(index, pathLength, pValue->getDocument());
        }

        if (type == Array) {
            /*
              We're going to repeat this for each member of the array,
              building up a new array as we go.
            */
            vector<intrusive_ptr<const Value> > result;
            intrusive_ptr<ValueIterator> pIter(pValue->getArray());
            while(pIter->more()) {
                intrusive_ptr<const Value> pItem(pIter->next());
                BSONType iType = pItem->getType();
                if ((iType == Undefined) || (iType == jstNULL)) {
                    result.push_back(pItem);
                    continue;
                }

                uassert(16014, str::stream() << 
                        "the element \"" << fieldPath.getFieldName(index) <<
                        "\" along the dotted path \"" <<
                        fieldPath.getPath(false) <<
                        "\" is not an object, and cannot be navigated",
                        iType == Object);
                intrusive_ptr<const Value> itemResult(
                    evaluatePath(index, pathLength, pItem->getDocument()));
                result.push_back(itemResult);
            }

            return Value::createArray(result);
        }

        uassert(16015, str::stream() <<
                "can't navigate into value of type " << type <<
                "at \"" << fieldPath.getFieldName(index) <<
                "\" in dotted path \"" << fieldPath.getPath(false),
                false);
        return intrusive_ptr<const Value>();
    }

    intrusive_ptr<const Value> ExpressionFieldPath::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        return evaluatePath(0, fieldPath.getPathLength(), pDocument);
    }

    void ExpressionFieldPath::addToBsonObj(
        BSONObjBuilder *pBuilder, string fieldName,
        bool requireExpression) const {
        pBuilder->append(fieldName, fieldPath.getPath(true));
    }

    void ExpressionFieldPath::addToBsonArray(
        BSONArrayBuilder *pBuilder) const {
        pBuilder->append(getFieldPath(true));
    }

    /* --------------------- ExpressionFieldRange -------------------------- */

    ExpressionFieldRange::~ExpressionFieldRange() {
    }

    intrusive_ptr<Expression> ExpressionFieldRange::optimize() {
        /* if there is no range to match, this will never evaluate true */
        if (!pRange.get())
            return ExpressionConstant::create(Value::getFalse());

        /*
          If we ended up with a double un-ended range, anything matches.  I
          don't know how that can happen, given intersect()'s interface, but
          here it is, just in case.
        */
        if (!pRange->pBottom.get() && !pRange->pTop.get())
            return ExpressionConstant::create(Value::getTrue());

        /*
          In all other cases, we have to test candidate values.  The
          intersect() method has already optimized those tests, so there
          aren't any more optimizations to look for here.
        */
        return intrusive_ptr<Expression>(this);
    }

    void ExpressionFieldRange::addDependencies(
        const intrusive_ptr<DependencyTracker> &pTracker,
        const DocumentSource *pSource) const {
        pFieldPath->addDependencies(pTracker, pSource);
    }

    intrusive_ptr<const Value> ExpressionFieldRange::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        /* if there's no range, there can't be a match */
        if (!pRange.get())
            return Value::getFalse();

        /* get the value of the specified field */
        intrusive_ptr<const Value> pValue(pFieldPath->evaluate(pDocument));

        /* see if it fits within any of the ranges */
        if (pRange->contains(pValue))
            return Value::getTrue();

        return Value::getFalse();
    }

    void ExpressionFieldRange::addToBson(Builder *pBuilder) const {
        if (!pRange.get()) {
            /* nothing will satisfy this predicate */
            pBuilder->append(false);
            return;
        }

        if (!pRange->pTop.get() && !pRange->pBottom.get()) {
            /* any value will satisfy this predicate */
            pBuilder->append(true);
            return;
        }

        if (pRange->pTop.get() == pRange->pBottom.get()) {
            BSONArrayBuilder operands;
            pFieldPath->addToBsonArray(&operands);
            pRange->pTop->addToBsonArray(&operands);
            
            BSONObjBuilder equals;
            equals.append("$eq", operands.arr());
            pBuilder->append(&equals);
            return;
        }

        BSONObjBuilder leftOperator;
        if (pRange->pBottom.get()) {
            BSONArrayBuilder leftOperands;
            pFieldPath->addToBsonArray(&leftOperands);
            pRange->pBottom->addToBsonArray(&leftOperands);
            leftOperator.append(
                (pRange->bottomOpen ? "$gt" : "$gte"),
                leftOperands.arr());

            if (!pRange->pTop.get()) {
                pBuilder->append(&leftOperator);
                return;
            }
        }

        BSONObjBuilder rightOperator;
        if (pRange->pTop.get()) {
            BSONArrayBuilder rightOperands;
            pFieldPath->addToBsonArray(&rightOperands);
            pRange->pTop->addToBsonArray(&rightOperands);
            rightOperator.append(
                (pRange->topOpen ? "$lt" : "$lte"),
                rightOperands.arr());

            if (!pRange->pBottom.get()) {
                pBuilder->append(&rightOperator);
                return;
            }
        }

        BSONArrayBuilder andOperands;
        andOperands.append(leftOperator.done());
        andOperands.append(rightOperator.done());
        BSONObjBuilder andOperator;
        andOperator.append("$and", andOperands.arr());
        pBuilder->append(&andOperator);
    }

    void ExpressionFieldRange::addToBsonObj(
        BSONObjBuilder *pBuilder, string fieldName,
        bool requireExpression) const {
        BuilderObj builder(pBuilder, fieldName);
        addToBson(&builder);
    }

    void ExpressionFieldRange::addToBsonArray(
        BSONArrayBuilder *pBuilder) const {
        BuilderArray builder(pBuilder);
        addToBson(&builder);
    }

    void ExpressionFieldRange::toMatcherBson(
        BSONObjBuilder *pBuilder) const {
        verify(pRange.get()); // otherwise, we can't do anything

        /* if there are no endpoints, then every value is accepted */
        if (!pRange->pBottom.get() && !pRange->pTop.get())
            return; // nothing to add to the predicate

        /* we're going to need the field path */
        string fieldPath(pFieldPath->getFieldPath(false));

        BSONObjBuilder range;
        if (pRange->pBottom.get()) {
            /* the test for equality doesn't generate a subobject */
            if (pRange->pBottom.get() == pRange->pTop.get()) {
                pRange->pBottom->addToBsonObj(pBuilder, fieldPath);
                return;
            }

            pRange->pBottom->addToBsonObj(
                pBuilder, (pRange->bottomOpen ? "$gt" : "$gte"));
        }

        if (pRange->pTop.get()) {
            pRange->pTop->addToBsonObj(
                pBuilder, (pRange->topOpen ? "$lt" : "$lte"));
        }

        pBuilder->append(fieldPath, range.done());
    }

    intrusive_ptr<ExpressionFieldRange> ExpressionFieldRange::create(
        const intrusive_ptr<ExpressionFieldPath> &pFieldPath, CmpOp cmpOp,
        const intrusive_ptr<const Value> &pValue) {
        intrusive_ptr<ExpressionFieldRange> pE(
            new ExpressionFieldRange(pFieldPath, cmpOp, pValue));
        return pE;
    }

    ExpressionFieldRange::ExpressionFieldRange(
        const intrusive_ptr<ExpressionFieldPath> &pTheFieldPath, CmpOp cmpOp,
        const intrusive_ptr<const Value> &pValue):
        pFieldPath(pTheFieldPath),
        pRange(new Range(cmpOp, pValue)) {
    }

    void ExpressionFieldRange::intersect(
        CmpOp cmpOp, const intrusive_ptr<const Value> &pValue) {

        /* create the new range */
        scoped_ptr<Range> pNew(new Range(cmpOp, pValue));

        /*
          Go through the range list.  For every range, either add the
          intersection of that to the range list, or if there is none, the
          original range.  This has the effect of restricting overlapping
          ranges, but leaving non-overlapping ones as-is.
        */
        pRange.reset(pRange->intersect(pNew.get()));
    }

    ExpressionFieldRange::Range::Range(
        CmpOp cmpOp, const intrusive_ptr<const Value> &pValue):
        bottomOpen(false),
        topOpen(false),
        pBottom(),
        pTop() {
        switch(cmpOp) {
        case NE:
            bottomOpen = topOpen = true;
            /* FALLTHROUGH */
        case EQ:
            pBottom = pTop = pValue;
            break;

        case GT:
            bottomOpen = true;
            /* FALLTHROUGH */
        case GTE:
            topOpen = true;
            pBottom = pValue;
            break;

        case LT:
            topOpen = true;
            /* FALLTHROUGH */
        case LTE:
            bottomOpen = true;
            pTop = pValue;
            break;

        case CMP:
            verify(false); // not allowed
            break;
        }
    }

    ExpressionFieldRange::Range::Range(const Range &rRange):
        bottomOpen(rRange.bottomOpen),
        topOpen(rRange.topOpen),
        pBottom(rRange.pBottom),
        pTop(rRange.pTop) {
    }

    ExpressionFieldRange::Range::Range(
        const intrusive_ptr<const Value> &pTheBottom, bool theBottomOpen,
        const intrusive_ptr<const Value> &pTheTop, bool theTopOpen):
        bottomOpen(theBottomOpen),
        topOpen(theTopOpen),
        pBottom(pTheBottom),
        pTop(pTheTop) {
    }
        
    ExpressionFieldRange::Range *ExpressionFieldRange::Range::intersect(
        const Range *pRange) const {
        /*
          Find the max of the bottom end of the ranges.

          Start by assuming the maximum is from pRange.  Then, if we have
          values of our own, see if they're greater.
        */
        intrusive_ptr<const Value> pMaxBottom(pRange->pBottom);
        bool maxBottomOpen = pRange->bottomOpen;
        if (pBottom.get()) {
            if (!pRange->pBottom.get()) {
                pMaxBottom = pBottom;
                maxBottomOpen = bottomOpen;
            }
            else {
                const int cmp = Value::compare(pBottom, pRange->pBottom);
                if (cmp == 0)
                    maxBottomOpen = bottomOpen || pRange->bottomOpen;
                else if (cmp > 0) {
                    pMaxBottom = pBottom;
                    maxBottomOpen = bottomOpen;
                }
            }
        }

        /*
          Find the minimum of the tops of the ranges.

          Start by assuming the minimum is from pRange.  Then, if we have
          values of our own, see if they are less.
        */
        intrusive_ptr<const Value> pMinTop(pRange->pTop);
        bool minTopOpen = pRange->topOpen;
        if (pTop.get()) {
            if (!pRange->pTop.get()) {
                pMinTop = pTop;
                minTopOpen = topOpen;
            }
            else {
                const int cmp = Value::compare(pTop, pRange->pTop);
                if (cmp == 0)
                    minTopOpen = topOpen || pRange->topOpen;
                else if (cmp < 0) {
                    pMinTop = pTop;
                    minTopOpen = topOpen;
                }
            }
        }

        /*
          If the intersections didn't create a disjoint set, create the
          new range.
        */
        if (Value::compare(pMaxBottom, pMinTop) <= 0)
            return new Range(pMaxBottom, maxBottomOpen, pMinTop, minTopOpen);

        /* if we got here, the intersection is empty */
        return NULL;
    }

    bool ExpressionFieldRange::Range::contains(
        const intrusive_ptr<const Value> &pValue) const {
        if (pBottom.get()) {
            const int cmp = Value::compare(pValue, pBottom);
            if (cmp < 0)
                return false;
            if (bottomOpen && (cmp == 0))
                return false;
        }

        if (pTop.get()) {
            const int cmp = Value::compare(pValue, pTop);
            if (cmp > 0)
                return false;
            if (topOpen && (cmp == 0))
                return false;
        }

        return true;
    }

    /* ------------------------- ExpressionIsoDate ------------------------- */

    const char ExpressionIsoDate::argYear[] = "year";
    const char ExpressionIsoDate::argMonth[] = "month";
    const char ExpressionIsoDate::argDayOfMonth[] = "dayOfMonth";
    const char ExpressionIsoDate::argHour[] = "hour";
    const char ExpressionIsoDate::argMinute[] = "minute";
    const char ExpressionIsoDate::argSecond[] = "second";

    const unsigned ExpressionIsoDate::flagYear = 0x0001;
    const unsigned ExpressionIsoDate::flagMonth = 0x0002;
    const unsigned ExpressionIsoDate::flagDayOfMonth = 0x0004;
    const unsigned ExpressionIsoDate::flagHour = 0x0008;
    const unsigned ExpressionIsoDate::flagMinute = 0x0010;
    const unsigned ExpressionIsoDate::flagSecond = 0x0020;


    ExpressionIsoDate::~ExpressionIsoDate() {
    }

    intrusive_ptr<ExpressionNary> ExpressionIsoDate::create() {
        intrusive_ptr<ExpressionIsoDate> pExpression(new ExpressionIsoDate());
        return pExpression;
    }

    ExpressionIsoDate::ExpressionIsoDate():
        ExpressionNary(),
        flag(0) {
    }

    void ExpressionIsoDate::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);

        ExpressionObject *pEO = dynamic_cast<ExpressionObject *>(
            pExpression.get());
        uassert(16023, str::stream() << getOpName() << 
                " expects an object as an argument", pEO);

        /* check pExpression's component parts to be sure they're valid */
        intrusive_ptr<Iterator<string> > is(pEO->getFieldIterator());
        while(is->hasNext()) {
            string fieldName(is->next());

            if (!fieldName.compare(argYear)) {
                flag |= ExpressionIsoDate::flagYear;
                continue;
            }

            if (!fieldName.compare(argMonth)) {
                flag |= ExpressionIsoDate::flagMonth;
                continue;
            }

            if (!fieldName.compare(argDayOfMonth)) {
                flag |= ExpressionIsoDate::flagDayOfMonth;
                continue;
            }

            if (!fieldName.compare(argHour)) {
                flag |= ExpressionIsoDate::flagHour;
                continue;
            }

            if (!fieldName.compare(argMinute)) {
                flag |= ExpressionIsoDate::flagMinute;
                continue;
            }

            if (!fieldName.compare(argSecond)) {
                flag |= ExpressionIsoDate::flagSecond;
                continue;
            }

            uassert(16024, str::stream() << getOpName() <<
                    ":  unrecognized named operand \"" <<
                    fieldName << "\"", false);
        }

        /* if we got here, all is well, and we can add the argument */
        ExpressionNary::addOperand(pExpression);
    }

    int ExpressionIsoDate::checkIntRange(const char *pName, long long value) const {
        uassert(16027, str::stream() << getOpName() <<
                ":  \"" << pName <<
                "\" value outside of range INT_MIN to INT_MAX",
                (value < INT_MIN) | (value > INT_MAX));

        return static_cast<int>(value);
    }

    int ExpressionIsoDate::getIntArg(
        const intrusive_ptr<Document> &pArgs,
        const char *pName, int defaultValue) const {
        intrusive_ptr<const Value> arg(
            pArgs->getField(pName));

        if (!arg.get())
            return defaultValue;

        BSONType type = arg->getType();
        if (type == jstNULL)
            return defaultValue;

        if (arg->getType() == NumberInt)
            return arg->getInt();

        if (arg->getType() == NumberLong)
            return checkIntRange(pName, arg->getLong());

        if (arg->getType() == NumberDouble) {
            double d = arg->getDouble();
            long l = static_cast<long>(d);
            uassert(16025, str::stream() << getOpName() <<
                    ":  \"" << pName << "\" operand is not a whole number",
                    l != d);

            return checkIntRange(pName, l);
        }

        uassert(16026, str::stream() << getOpName() <<
                    ":  \"" << pName << "\" operand is not a number",
                    false);
        /* NOTREACHED */
        return 0;
    }

    /**
       Amazingly, there is no inverse of gmtime(), which is what we used to
       decompose a time_t to a struct tm.  mktime() only converts to local
       time.

       After digging around for quite some time, finally found an indirect
       solution in the boost POSIX time libraries.

       http://stackoverflow.com/a/1039370/857029
       http://stackoverflow.com/a/6849810/857029
    */
    static unsigned long long datetFromUtcTm(const tm *pTm) {
        using namespace boost::posix_time;
        static ptime epoch(boost::gregorian::date(1970, 1, 1));

        ptime pt(boost::gregorian::date(
                     pTm->tm_year + 1900, pTm->tm_mon + 1, pTm->tm_mday),
                 time_duration(pTm->tm_hour, pTm->tm_min, pTm->tm_sec));
        time_duration diff(pt - epoch);
        return (diff.ticks()/diff.ticks_per_second()) * 1000;
    }

    intrusive_ptr<const Value> ExpressionIsoDate::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        /*
          If the argument object is set, then addOperand() will have checked
          to make sure the field combinations are valid.  However, the types
          of those fields still need to be checked below.
         */
        checkArgCount(1);

        /* evaluate all the date parts */
        intrusive_ptr<const Value> pDate(vpOperand[0]->evaluate(pDocument));
        intrusive_ptr<Document> pArgs(pDate->getDocument());
        

        tm date;

        /* set the default values */
        date.tm_sec = 0;
        date.tm_min = 0;
        date.tm_hour = 0;
        date.tm_mday = 1;
        date.tm_mon = 0;
        date.tm_year = 0;
        date.tm_isdst = -1; /* don't know if this is DST or not */

        /* set the named parameters */
        if (flag & ExpressionIsoDate::flagYear)
            date.tm_year =
                getIntArg(pArgs, ExpressionIsoDate::argYear, 1900) - 1900;

        if (flag & ExpressionIsoDate::flagMonth)
            date.tm_mon =
                getIntArg(pArgs, ExpressionIsoDate::argMonth, 1) - 1;

        if (flag & ExpressionIsoDate::flagDayOfMonth)
            date.tm_mday = getIntArg(pArgs, ExpressionIsoDate::argDayOfMonth, 1);

        if (flag & ExpressionIsoDate::flagHour)
            date.tm_hour = getIntArg(pArgs, ExpressionIsoDate::argHour, 0);

        if (flag & ExpressionIsoDate::flagMinute)
            date.tm_min = getIntArg(pArgs, ExpressionIsoDate::argMinute, 0);

        if (flag & ExpressionIsoDate::flagSecond)
            date.tm_sec = getIntArg(pArgs, ExpressionIsoDate::argSecond, 0);

        Date_t d(datetFromUtcTm(&date));
        return Value::createDate(d);
    }

    const char *ExpressionIsoDate::getOpName() const {
        return "$isoDate";
    }

    /* ------------------------- ExpressionMinute -------------------------- */

    ExpressionMinute::~ExpressionMinute() {
    }

    intrusive_ptr<ExpressionNary> ExpressionMinute::create() {
        intrusive_ptr<ExpressionMinute> pExpression(new ExpressionMinute());
        return pExpression;
    }

    ExpressionMinute::ExpressionMinute():
        ExpressionNary() {
    }

    void ExpressionMinute::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionMinute::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(1);
        intrusive_ptr<const Value> pDate(vpOperand[0]->evaluate(pDocument));
        tm date;
        (pDate->coerceToDate()).toTm(&date);
        return Value::createInt(date.tm_min);
    }

    const char *ExpressionMinute::getOpName() const {
        return "$minute";
    }

    /* ----------------------- ExpressionMod ---------------------------- */

    ExpressionMod::~ExpressionMod() {
    }

    intrusive_ptr<ExpressionNary> ExpressionMod::create() {
        intrusive_ptr<ExpressionMod> pExpression(new ExpressionMod());
        return pExpression;
    }

    ExpressionMod::ExpressionMod():
        ExpressionNary() {
    }

    void ExpressionMod::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(2);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionMod::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        BSONType productType;
        checkArgCount(2);
        intrusive_ptr<const Value> pLeft(vpOperand[0]->evaluate(pDocument));
        intrusive_ptr<const Value> pRight(vpOperand[1]->evaluate(pDocument));

        productType = Value::getWidestNumeric(pRight->getType(), pLeft->getType());

        long long right = pRight->coerceToLong();
        if (right == 0)
            return Value::getUndefined();

        long long left = pLeft->coerceToLong();
        if (productType == NumberLong)
            return Value::createLong(left % right);
        return Value::createInt((int)left % right);
    }

    const char *ExpressionMod::getOpName() const {
        return "$mod";
    }

    /* ------------------------ ExpressionMonth ----------------------------- */

    ExpressionMonth::~ExpressionMonth() {
    }

    intrusive_ptr<ExpressionNary> ExpressionMonth::create() {
        intrusive_ptr<ExpressionMonth> pExpression(new ExpressionMonth());
        return pExpression;
    }

    ExpressionMonth::ExpressionMonth():
        ExpressionNary() {
    }

    void ExpressionMonth::addOperand(const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionMonth::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(1);
        intrusive_ptr<const Value> pDate(vpOperand[0]->evaluate(pDocument));
        tm date;
        (pDate->coerceToDate()).toTm(&date);
        return Value::createInt(date.tm_mon + 1); // MySQL uses 1-12 tm uses 0-11
    }

    const char *ExpressionMonth::getOpName() const {
        return "$month";
    }

    /* ------------------------- ExpressionMultiply ----------------------------- */

    ExpressionMultiply::~ExpressionMultiply() {
    }

    intrusive_ptr<ExpressionNary> ExpressionMultiply::create() {
        intrusive_ptr<ExpressionMultiply> pExpression(new ExpressionMultiply());
        return pExpression;
    }

    ExpressionMultiply::ExpressionMultiply():
        ExpressionNary() {
    }

    intrusive_ptr<const Value> ExpressionMultiply::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        /*
          We'll try to return the narrowest possible result value.  To do that
          without creating intermediate Values, do the arithmetic for double
          and integral types in parallel, tracking the current narrowest
          type.
         */
        double doubleProduct = 1;
        long long longProduct = 1;
        BSONType productType = NumberInt;

        const size_t n = vpOperand.size();
        for(size_t i = 0; i < n; ++i) {
            intrusive_ptr<const Value> pValue(vpOperand[i]->evaluate(pDocument));

            productType = Value::getWidestNumeric(productType, pValue->getType());
            doubleProduct *= pValue->coerceToDouble();
            longProduct *= pValue->coerceToLong();
        }

        if (productType == NumberDouble)
            return Value::createDouble(doubleProduct);
        if (productType == NumberLong)
            return Value::createLong(longProduct);
        return Value::createInt((int)longProduct);
    }

    const char *ExpressionMultiply::getOpName() const {
    return "$multiply";
    }

    intrusive_ptr<ExpressionNary> (*ExpressionMultiply::getFactory() const)() {
    return ExpressionMultiply::create;
    }

    /* ------------------------- ExpressionHour ----------------------------- */

    ExpressionHour::~ExpressionHour() {
    }

    intrusive_ptr<ExpressionNary> ExpressionHour::create() {
        intrusive_ptr<ExpressionHour> pExpression(new ExpressionHour());
        return pExpression;
    }

    ExpressionHour::ExpressionHour():
        ExpressionNary() {
    }

    void ExpressionHour::addOperand(const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionHour::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(1);
        intrusive_ptr<const Value> pDate(vpOperand[0]->evaluate(pDocument));
        tm date;
        (pDate->coerceToDate()).toTm(&date);
        return Value::createInt(date.tm_hour);
    }

    const char *ExpressionHour::getOpName() const {
        return "$hour";
    }

    /* ----------------------- ExpressionIfNull ---------------------------- */

    ExpressionIfNull::~ExpressionIfNull() {
    }

    intrusive_ptr<ExpressionNary> ExpressionIfNull::create() {
        intrusive_ptr<ExpressionIfNull> pExpression(new ExpressionIfNull());
        return pExpression;
    }

    ExpressionIfNull::ExpressionIfNull():
        ExpressionNary() {
    }

    void ExpressionIfNull::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(2);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionIfNull::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(2);
        intrusive_ptr<const Value> pLeft(vpOperand[0]->evaluate(pDocument));
        BSONType leftType = pLeft->getType();

        if ((leftType != Undefined) && (leftType != jstNULL))
            return pLeft;

        intrusive_ptr<const Value> pRight(vpOperand[1]->evaluate(pDocument));
        return pRight;
    }

    const char *ExpressionIfNull::getOpName() const {
        return "$ifNull";
    }

    /* ------------------------ ExpressionNary ----------------------------- */

    ExpressionNary::ExpressionNary():
        vpOperand() {
    }

    intrusive_ptr<Expression> ExpressionNary::optimize() {
        unsigned constCount = 0; // count of constant operands
        unsigned stringCount = 0; // count of constant string operands
        const size_t n = vpOperand.size();
        for(size_t i = 0; i < n; ++i) {
            intrusive_ptr<Expression> pNew(vpOperand[i]->optimize());

            /* subsitute the optimized expression */
            vpOperand[i] = pNew;

            /* check to see if the result was a constant */
            const ExpressionConstant *pConst =
                dynamic_cast<ExpressionConstant *>(pNew.get());
            if (pConst) {
                ++constCount;
                if (pConst->getValue()->getType() == String)
                    ++stringCount;
            }
        }

        /*
          If all the operands are constant, we can replace this expression
          with a constant.  We can find the value by evaluating this
          expression over a NULL Document because evaluating the
          ExpressionConstant never refers to the argument Document.
        */
        if (constCount == n) {
            intrusive_ptr<const Value> pResult(
                evaluate(intrusive_ptr<Document>()));
            intrusive_ptr<Expression> pReplacement(
                ExpressionConstant::create(pResult));
            return pReplacement;
        }

        /*
          If there are any strings, we can't re-arrange anything, so stop
          now.

          LATER:  we could concatenate adjacent strings as a special case.
         */
        if (stringCount)
            return intrusive_ptr<Expression>(this);

        /*
          If there's no more than one constant, then we can't do any
          constant folding, so don't bother going any further.
         */
        if (constCount <= 1)
            return intrusive_ptr<Expression>(this);
            
        /*
          If the operator isn't commutative or associative, there's nothing
          more we can do.  We test that by seeing if we can get a factory;
          if we can, we can use it to construct a temporary expression which
          we'll evaluate to collapse as many constants as we can down to
          a single one.
         */
        intrusive_ptr<ExpressionNary> (*const pFactory)() = getFactory();
        if (!pFactory)
            return intrusive_ptr<Expression>(this);

        /*
          Create a new Expression that will be the replacement for this one.
          We actually create two:  one to hold constant expressions, and
          one to hold non-constants.  Once we've got these, we evaluate
          the constant expression to produce a single value, as above.
          We then add this operand to the end of the non-constant expression,
          and return that.
         */
        intrusive_ptr<ExpressionNary> pNew((*pFactory)());
        intrusive_ptr<ExpressionNary> pConst((*pFactory)());
        for(size_t i = 0; i < n; ++i) {
            intrusive_ptr<Expression> pE(vpOperand[i]);
            if (dynamic_cast<ExpressionConstant *>(pE.get()))
                pConst->addOperand(pE);
            else {
                /*
                  If the child operand is the same type as this, then we can
                  extract its operands and inline them here because we already
                  know this is commutative and associative because it has a
                  factory.  We can detect sameness of the child operator by
                  checking for equality of the factory

                  Note we don't have to do this recursively, because we
                  called optimize() on all the children first thing in
                  this call to optimize().
                */
                ExpressionNary *pNary =
                    dynamic_cast<ExpressionNary *>(pE.get());
                if (!pNary)
                    pNew->addOperand(pE);
                else {
                    intrusive_ptr<ExpressionNary> (*const pChildFactory)() =
                        pNary->getFactory();
                    if (pChildFactory != pFactory)
                        pNew->addOperand(pE);
                    else {
                        /* same factory, so flatten */
                        size_t nChild = pNary->vpOperand.size();
                        for(size_t iChild = 0; iChild < nChild; ++iChild) {
                            intrusive_ptr<Expression> pCE(
                                pNary->vpOperand[iChild]);
                            if (dynamic_cast<ExpressionConstant *>(pCE.get()))
                                pConst->addOperand(pCE);
                            else
                                pNew->addOperand(pCE);
                        }
                    }
                }
            }
        }

        /*
          If there was only one constant, add it to the end of the expression
          operand vector.
        */
        if (pConst->vpOperand.size() == 1)
            pNew->addOperand(pConst->vpOperand[0]);
        else if (pConst->vpOperand.size() > 1) {
            /*
              If there was more than one constant, collapse all the constants
              together before adding the result to the end of the expression
              operand vector.
            */
            intrusive_ptr<const Value> pResult(
                pConst->evaluate(intrusive_ptr<Document>()));
            pNew->addOperand(ExpressionConstant::create(pResult));
        }

        return pNew;
    }

    void ExpressionNary::addDependencies(
        const intrusive_ptr<DependencyTracker> &pTracker,
        const DocumentSource *pSource) const {
        for(ExpressionVector::const_iterator i(vpOperand.begin());
            i != vpOperand.end(); ++i) {
            (*i)->addDependencies(pTracker, pSource);
        }
    }

    void ExpressionNary::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        vpOperand.push_back(pExpression);
    }

    intrusive_ptr<ExpressionNary> (*ExpressionNary::getFactory() const)() {
        return NULL;
    }

    void ExpressionNary::toBson(
        BSONObjBuilder *pBuilder, const char *pOpName) const {
        const size_t nOperand = vpOperand.size();
        verify(nOperand > 0);
        if (nOperand == 1) {
            vpOperand[0]->addToBsonObj(pBuilder, pOpName, false);
            return;
        }

        /* build up the array */
        BSONArrayBuilder arrBuilder;
        for(size_t i = 0; i < nOperand; ++i)
            vpOperand[i]->addToBsonArray(&arrBuilder);

        pBuilder->append(pOpName, arrBuilder.arr());
    }

    void ExpressionNary::addToBsonObj(
        BSONObjBuilder *pBuilder, string fieldName,
        bool requireExpression) const {
        BSONObjBuilder exprBuilder;
        toBson(&exprBuilder, getOpName());
        pBuilder->append(fieldName, exprBuilder.done());
    }

    void ExpressionNary::addToBsonArray(
        BSONArrayBuilder *pBuilder) const {
        BSONObjBuilder exprBuilder;
        toBson(&exprBuilder, getOpName());
        pBuilder->append(exprBuilder.done());
    }

    void ExpressionNary::checkArgLimit(unsigned maxArgs) const {
        uassert(15993, str::stream() << getOpName() <<
                " only takes " << maxArgs <<
                " operand" << (maxArgs == 1 ? "" : "s"),
                vpOperand.size() < maxArgs);
    }

    void ExpressionNary::checkArgCount(unsigned reqArgs) const {
        uassert(15997, str::stream() << getOpName() <<
                ":  insufficient operands; " << reqArgs <<
                " required, only got " << vpOperand.size(),
                vpOperand.size() == reqArgs);
    }

    /* ----------------------- ExpressionNoOp ------------------------------ */

    ExpressionNoOp::~ExpressionNoOp() {
    }

    intrusive_ptr<ExpressionNary> ExpressionNoOp::create() {
        intrusive_ptr<ExpressionNoOp> pExpression(new ExpressionNoOp());
        return pExpression;
    }

    intrusive_ptr<Expression> ExpressionNoOp::optimize() {
        checkArgCount(1);
        intrusive_ptr<Expression> pR(vpOperand[0]->optimize());
        return pR;
    }

    ExpressionNoOp::ExpressionNoOp():
        ExpressionNary() {
    }

    void ExpressionNoOp::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionNoOp::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(1);
        intrusive_ptr<const Value> pValue(vpOperand[0]->evaluate(pDocument));
        return pValue;
    }

    const char *ExpressionNoOp::getOpName() const {
        return "$noOp";
    }

    /* ------------------------- ExpressionNot ----------------------------- */

    ExpressionNot::~ExpressionNot() {
    }

    intrusive_ptr<ExpressionNary> ExpressionNot::create() {
        intrusive_ptr<ExpressionNot> pExpression(new ExpressionNot());
        return pExpression;
    }

    ExpressionNot::ExpressionNot():
        ExpressionNary() {
    }

    void ExpressionNot::addOperand(const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionNot::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(1);
        intrusive_ptr<const Value> pOp(vpOperand[0]->evaluate(pDocument));

        bool b = pOp->coerceToBool();
        if (b)
            return Value::getFalse();
        return Value::getTrue();
    }

    const char *ExpressionNot::getOpName() const {
        return "$not";
    }

    /* -------------------------- ExpressionOr ----------------------------- */

    ExpressionOr::~ExpressionOr() {
    }

    intrusive_ptr<ExpressionNary> ExpressionOr::create() {
        intrusive_ptr<ExpressionNary> pExpression(new ExpressionOr());
        return pExpression;
    }

    ExpressionOr::ExpressionOr():
        ExpressionNary() {
    }

    intrusive_ptr<const Value> ExpressionOr::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        const size_t n = vpOperand.size();
        for(size_t i = 0; i < n; ++i) {
            intrusive_ptr<const Value> pValue(vpOperand[i]->evaluate(pDocument));
            if (pValue->coerceToBool())
                return Value::getTrue();
        }

        return Value::getFalse();
    }

    void ExpressionOr::toMatcherBson(
        BSONObjBuilder *pBuilder) const {
        BSONObjBuilder opArray;
        const size_t n = vpOperand.size();
        for(size_t i = 0; i < n; ++i)
            vpOperand[i]->toMatcherBson(&opArray);

        pBuilder->append("$or", opArray.done());
    }

    intrusive_ptr<ExpressionNary> (*ExpressionOr::getFactory() const)() {
        return ExpressionOr::create;
    }

    intrusive_ptr<Expression> ExpressionOr::optimize() {
        /* optimize the disjunction as much as possible */
        intrusive_ptr<Expression> pE(ExpressionNary::optimize());

        /* if the result isn't a conjunction, we can't do anything */
        ExpressionOr *pOr = dynamic_cast<ExpressionOr *>(pE.get());
        if (!pOr)
            return pE;

        /*
          Check the last argument on the result; if it's not constant (as
          promised by ExpressionNary::optimize(),) then there's nothing
          we can do.
        */
        const size_t n = pOr->vpOperand.size();
        intrusive_ptr<Expression> pLast(pOr->vpOperand[n - 1]);
        const ExpressionConstant *pConst =
            dynamic_cast<ExpressionConstant *>(pLast.get());
        if (!pConst)
            return pE;

        /*
          Evaluate and coerce the last argument to a boolean.  If it's true,
          then we can replace this entire expression.
         */
        bool last = pLast->evaluate(intrusive_ptr<Document>())->coerceToBool();
        if (last) {
            intrusive_ptr<ExpressionConstant> pFinal(
                ExpressionConstant::create(Value::getTrue()));
            return pFinal;
        }

        /*
          If we got here, the final operand was false, so we don't need it
          anymore.  If there was only one other operand, we don't need the
          conjunction either.  Note we still need to keep the promise that
          the result will be a boolean.
         */
        if (n == 2) {
            intrusive_ptr<Expression> pFinal(
                ExpressionCoerceToBool::create(pOr->vpOperand[0]));
            return pFinal;
        }

        /*
          Remove the final "false" value, and return the new expression.
        */
        pOr->vpOperand.resize(n - 1);
        return pE;
    }

    const char *ExpressionOr::getOpName() const {
        return "$or";
    }

    /* ------------------------- ExpressionSecond ----------------------------- */

    ExpressionSecond::~ExpressionSecond() {
    }

    intrusive_ptr<ExpressionNary> ExpressionSecond::create() {
        intrusive_ptr<ExpressionSecond> pExpression(new ExpressionSecond());
        return pExpression;
    }

    ExpressionSecond::ExpressionSecond():
        ExpressionNary() {
    }

    void ExpressionSecond::addOperand(const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionSecond::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(1);
        intrusive_ptr<const Value> pDate(vpOperand[0]->evaluate(pDocument));
        tm date;
        (pDate->coerceToDate()).toTm(&date);
        return Value::createInt(date.tm_sec);
    }

    const char *ExpressionSecond::getOpName() const {
        return "$second";
    }

    /* ----------------------- ExpressionStrcasecmp ---------------------------- */

    ExpressionStrcasecmp::~ExpressionStrcasecmp() {
    }

    intrusive_ptr<ExpressionNary> ExpressionStrcasecmp::create() {
        intrusive_ptr<ExpressionStrcasecmp> pExpression(new ExpressionStrcasecmp());
        return pExpression;
    }

    ExpressionStrcasecmp::ExpressionStrcasecmp():
        ExpressionNary() {
    }

    void ExpressionStrcasecmp::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(2);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionStrcasecmp::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(2);
        intrusive_ptr<const Value> pString1(vpOperand[0]->evaluate(pDocument));
        intrusive_ptr<const Value> pString2(vpOperand[1]->evaluate(pDocument));

        /* boost::iequals returns a bool not an int so strings must actually be allocated */
        string str1 = boost::to_upper_copy( pString1->coerceToString() );
        string str2 = boost::to_upper_copy( pString2->coerceToString() );
        int result = str1.compare(str2);

        if (result == 0)
            return Value::getZero();
        if (result > 0)
            return Value::getOne();
        return Value::getMinusOne();
    }

    const char *ExpressionStrcasecmp::getOpName() const {
        return "$strcasecmp";
    }

    /* ----------------------- ExpressionSubstr ---------------------------- */

    ExpressionSubstr::~ExpressionSubstr() {
    }

    intrusive_ptr<ExpressionNary> ExpressionSubstr::create() {
        intrusive_ptr<ExpressionSubstr> pExpression(new ExpressionSubstr());
        return pExpression;
    }

    ExpressionSubstr::ExpressionSubstr():
        ExpressionNary() {
    }

    void ExpressionSubstr::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(3);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionSubstr::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(3);
        intrusive_ptr<const Value> pString(vpOperand[0]->evaluate(pDocument));
        intrusive_ptr<const Value> pLower(vpOperand[1]->evaluate(pDocument));
        intrusive_ptr<const Value> pLength(vpOperand[2]->evaluate(pDocument));

        string str = pString->coerceToString();
        uassert(16034, str::stream() << getOpName() <<
                ":  starting index must be a numeric type (is BSON type " <<
                pLower->getType() << ")",
                (pLower->getType() == NumberInt 
                 || pLower->getType() == NumberLong 
                 || pLower->getType() == NumberDouble));
        uassert(16035, str::stream() << getOpName() <<
                ":  length must be a numeric type (is BSON type " <<
                pLength->getType() << ")",
                (pLength->getType() == NumberInt 
                 || pLength->getType() == NumberLong 
                 || pLength->getType() == NumberDouble));
        string::size_type lower = static_cast< string::size_type >( pLower->coerceToLong() );
        string::size_type length = static_cast< string::size_type >( pLength->coerceToLong() );
        return Value::createString( str.substr(lower, length) );
    }

    const char *ExpressionSubstr::getOpName() const {
        return "$substr";
    }

    /* ----------------------- ExpressionSubtract ---------------------------- */

    ExpressionSubtract::~ExpressionSubtract() {
    }

    intrusive_ptr<ExpressionNary> ExpressionSubtract::create() {
        intrusive_ptr<ExpressionSubtract> pExpression(new ExpressionSubtract());
        return pExpression;
    }

    ExpressionSubtract::ExpressionSubtract():
        ExpressionNary() {
    }

    void ExpressionSubtract::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(2);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionSubtract::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        BSONType productType;
        checkArgCount(2);
        intrusive_ptr<const Value> pLeft(vpOperand[0]->evaluate(pDocument));
        intrusive_ptr<const Value> pRight(vpOperand[1]->evaluate(pDocument));
        if (pLeft->getType() == Date) {
            long long right;
            long long left = pLeft->coerceToDate();
            if (pRight->getType() == Date)
                right = pRight->coerceToDate();
            else 
                right = static_cast<long long>(pRight->coerceToDouble()*24*60*60*1000);
            return Value::createDate(Date_t(left-right));
        }
            
        uassert(15996, "cannot subtract one date from another",
                pRight->getType() != Date);

        productType = Value::getWidestNumeric(
            pRight->getType(), pLeft->getType());
        

        if (productType == NumberDouble) {
            double right = pRight->coerceToDouble();
            double left = pLeft->coerceToDouble();
            return Value::createDouble(left - right);
        } 

        long long right = pRight->coerceToLong();
        long long left = pLeft->coerceToLong();
        if (productType == NumberLong)
            return Value::createLong(left - right);
        return Value::createInt((int)(left - right));
    }

    const char *ExpressionSubtract::getOpName() const {
        return "$subtract";
    }

    /* ------------------------- ExpressionToLower ----------------------------- */

    ExpressionToLower::~ExpressionToLower() {
    }

    intrusive_ptr<ExpressionNary> ExpressionToLower::create() {
        intrusive_ptr<ExpressionToLower> pExpression(new ExpressionToLower());
        return pExpression;
    }

    ExpressionToLower::ExpressionToLower():
        ExpressionNary() {
    }

    void ExpressionToLower::addOperand(const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionToLower::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(1);
        intrusive_ptr<const Value> pString(vpOperand[0]->evaluate(pDocument));
        string str = pString->coerceToString();
        boost::to_lower(str);
        return Value::createString(str);
    }

    const char *ExpressionToLower::getOpName() const {
        return "$toLower";
    }

    /* ------------------------- ExpressionToUpper -------------------------- */

    ExpressionToUpper::~ExpressionToUpper() {
    }

    intrusive_ptr<ExpressionNary> ExpressionToUpper::create() {
        intrusive_ptr<ExpressionToUpper> pExpression(new ExpressionToUpper());
        return pExpression;
    }

    ExpressionToUpper::ExpressionToUpper():
        ExpressionNary() {
    }

    void ExpressionToUpper::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionToUpper::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(1);
        intrusive_ptr<const Value> pString(vpOperand[0]->evaluate(pDocument));
        string str(pString->coerceToString());
        boost::to_upper(str);
        return Value::createString(str);
    }

    const char *ExpressionToUpper::getOpName() const {
        return "$toUpper";
    }

    /* ------------------------- ExpressionWeek ----------------------------- */

    ExpressionWeek::~ExpressionWeek() {
    }

    intrusive_ptr<ExpressionNary> ExpressionWeek::create() {
        intrusive_ptr<ExpressionWeek> pExpression(new ExpressionWeek());
        return pExpression;
    }

    ExpressionWeek::ExpressionWeek():
        ExpressionNary() {
    }

    void ExpressionWeek::addOperand(const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionWeek::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(1);
        intrusive_ptr<const Value> pDate(vpOperand[0]->evaluate(pDocument));
        tm date;
        (pDate->coerceToDate()).toTm(&date);
        int dayOfWeek = date.tm_wday+1;
        int dayOfYear = date.tm_yday;
        int week = 0;
        int janFirst = 0;
        int offset = 0;

        janFirst = dayOfWeek - dayOfYear % 7;
        offset = (janFirst + 6) % 7;
        week = (dayOfYear + offset) / 7;
        return Value::createInt(week);
    }

    const char *ExpressionWeek::getOpName() const {
        return "$week";
    }

    /* ------------------------- ExpressionYear ----------------------------- */

    ExpressionYear::~ExpressionYear() {
    }

    intrusive_ptr<ExpressionNary> ExpressionYear::create() {
        intrusive_ptr<ExpressionYear> pExpression(new ExpressionYear());
        return pExpression;
    }

    ExpressionYear::ExpressionYear():
        ExpressionNary() {
    }

    void ExpressionYear::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        checkArgLimit(1);
        ExpressionNary::addOperand(pExpression);
    }

    intrusive_ptr<const Value> ExpressionYear::evaluate(
        const intrusive_ptr<Document> &pDocument) const {
        checkArgCount(1);
        intrusive_ptr<const Value> pDate(vpOperand[0]->evaluate(pDocument));
        tm date;
        (pDate->coerceToDate()).toTm(&date);
        return Value::createInt(date.tm_year + 1900); // tm_year is years since 1900
    }

    const char *ExpressionYear::getOpName() const {
        return "$year";
    }

}
