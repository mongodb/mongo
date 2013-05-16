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
#include "db/pipeline/document.h"
#include "db/pipeline/expression_context.h"
#include "db/pipeline/value.h"
#include "util/mongoutils/str.h"

namespace mongo {
    using namespace mongoutils;

    /// Helper function to easily wrap constants with $const.
    static Value serializeConstant(Value val) {
        MutableDocument valBuilder;
        valBuilder["$const"] = val;
        return valBuilder.freezeToValue();
    }

    /* --------------------------- Expression ------------------------------ */

    void Expression::toMatcherBson(BSONObjBuilder *pBuilder) const {
        verify(false && "Expression::toMatcherBson()");
    }

    Expression::ObjectCtx::ObjectCtx(int theOptions)
        : options(theOptions)
    {}

    bool Expression::ObjectCtx::documentOk() const {
        return ((options & DOCUMENT_OK) != 0);
    }

    bool Expression::ObjectCtx::topLevel() const {
        return ((options & TOP_LEVEL) != 0);
    }

    bool Expression::ObjectCtx::inclusionOk() const {
        return ((options & INCLUSION_OK) != 0);
    }

    string Expression::removeFieldPrefix(const string &prefixedField) {
        uassert(16419, str::stream()<<"field path must not contain embedded null characters" << prefixedField.find("\0") << "," ,
                prefixedField.find('\0') == string::npos);

        const char *pPrefixedField = prefixedField.c_str();
        uassert(15982, str::stream() <<
                "field path references must be prefixed with a '$' ('" <<
                prefixedField << "'", pPrefixedField[0] == '$');

        return string(pPrefixedField + 1);
    }

    intrusive_ptr<Expression> Expression::parseObject(
        BSONElement *pBsonElement, ObjectCtx *pCtx) {
        /*
          An object expression can take any of the following forms:

          f0: {f1: ..., f2: ..., f3: ...}
          f0: {$operator:[operand1, operand2, ...]}
        */

        intrusive_ptr<Expression> pExpression; // the result
        intrusive_ptr<ExpressionObject> pExpressionObject; // alt result
        enum { UNKNOWN, NOTOPERATOR, OPERATOR } kind = UNKNOWN;

        BSONObj obj(pBsonElement->Obj());
        if (obj.isEmpty())
            return ExpressionObject::create();
        BSONObjIterator iter(obj);

        for(size_t fieldCount = 0; iter.more(); ++fieldCount) {
            BSONElement fieldElement(iter.next());
            const char *pFieldName = fieldElement.fieldName();

            if (pFieldName[0] == '$') {
                uassert(15983, str::stream() <<
                        "the operator must be the only field in a pipeline object (at '"
                        << pFieldName << "'",
                        fieldCount == 0);

                uassert(16404, "$expressions are not allowed at the top-level of $project",
                        !pCtx->topLevel());

                /* we've determined this "object" is an operator expression */
                kind = OPERATOR;

                pExpression = parseExpression(pFieldName, &fieldElement);
            }
            else {
                uassert(15990, str::stream() << "this object is already an operator expression, and can't be used as a document expression (at '" <<
                        pFieldName << "')",
                        kind != OPERATOR);

                uassert(16405, "dotted field names are only allowed at the top level",
                        pCtx->topLevel() || !str::contains(pFieldName, '.'));

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
                switch (fieldType){
                    case Object: {
                        /* it's a nested document */
                        ObjectCtx oCtx(
                            (pCtx->documentOk() ? ObjectCtx::DOCUMENT_OK : 0)
                             | (pCtx->inclusionOk() ? ObjectCtx::INCLUSION_OK : 0));
                        intrusive_ptr<Expression> pNested(
                            parseObject(&fieldElement, &oCtx));
                        pExpressionObject->addField(fieldName, pNested);
                        break;
                    }
                    case String: {
                        /* it's a renamed field */
                        // CW TODO could also be a constant
                        intrusive_ptr<Expression> pPath(
                            ExpressionFieldPath::create(
                                removeFieldPrefix(fieldElement.str())));
                        pExpressionObject->addField(fieldName, pPath);
                        break;
                    }
                    case Bool:
                    case NumberDouble:
                    case NumberLong:
                    case NumberInt: {
                        /* it's an inclusion specification */
                        if (fieldElement.trueValue()) {
                            uassert(16420, "field inclusion is not allowed inside of $expressions",
                                    pCtx->inclusionOk());
                            pExpressionObject->includePath(fieldName);
                        }
                        else {
                            uassert(16406,
                                    "The top-level _id field is the only field currently supported for exclusion",
                                    pCtx->topLevel() && fieldName == "_id");
                            pExpressionObject->excludeId(true);
                        }
                        break;
                    }
                    default:
                        uassert(15992, str::stream() <<
                                "disallowed field type " << typeName(fieldType) <<
                                " in object expression (at '" <<
                                fieldName << "')", false);
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
        {"$concat", ExpressionConcat::create, 0},
        {"$cond", ExpressionCond::create, OpDesc::FIXED_COUNT, 3},
        // $const handled specially in parseExpression
        {"$dayOfMonth", ExpressionDayOfMonth::create, OpDesc::FIXED_COUNT, 1},
        {"$dayOfWeek", ExpressionDayOfWeek::create, OpDesc::FIXED_COUNT, 1},
        {"$dayOfYear", ExpressionDayOfYear::create, OpDesc::FIXED_COUNT, 1},
        {"$divide", ExpressionDivide::create, OpDesc::FIXED_COUNT, 2},
        {"$eq", ExpressionCompare::createEq, OpDesc::FIXED_COUNT, 2},
        {"$gt", ExpressionCompare::createGt, OpDesc::FIXED_COUNT, 2},
        {"$gte", ExpressionCompare::createGte, OpDesc::FIXED_COUNT, 2},
        {"$hour", ExpressionHour::create, OpDesc::FIXED_COUNT, 1},
        {"$ifNull", ExpressionIfNull::create, OpDesc::FIXED_COUNT, 2},
        {"$lt", ExpressionCompare::createLt, OpDesc::FIXED_COUNT, 2},
        {"$lte", ExpressionCompare::createLte, OpDesc::FIXED_COUNT, 2},
        {"$millisecond", ExpressionMillisecond::create, OpDesc::FIXED_COUNT, 1},
        {"$minute", ExpressionMinute::create, OpDesc::FIXED_COUNT, 1},
        {"$mod", ExpressionMod::create, OpDesc::FIXED_COUNT, 2},
        {"$month", ExpressionMonth::create, OpDesc::FIXED_COUNT, 1},
        {"$multiply", ExpressionMultiply::create, 0},
        {"$ne", ExpressionCompare::createNe, OpDesc::FIXED_COUNT, 2},
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

        if (str::equals(pOpName, "$const")) {
            return ExpressionConstant::createFromBsonElement(pBsonElement);
        }

        OpDesc key;
        key.pName = pOpName;
        const OpDesc *pOp = (const OpDesc *)bsearch(
                                &key, OpTable, NOp, sizeof(OpDesc), OpDescCmp);

        uassert(15999, str::stream() << "invalid operator '" <<
                pOpName << "'", pOp);

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

    intrusive_ptr<Expression> Expression::parseOperand(BSONElement *pBsonElement) {
        BSONType type = pBsonElement->type();

        if (type == String && pBsonElement->valuestr()[0] == '$') {
            /* if we got here, this is a field path expression */
            string fieldPath = removeFieldPrefix(pBsonElement->str());
            return ExpressionFieldPath::create(fieldPath);
        }
        else if (type == Object) {
            ObjectCtx oCtx(ObjectCtx::DOCUMENT_OK);
            return Expression::parseObject(pBsonElement, &oCtx);
        }
        else {
            return ExpressionConstant::createFromBsonElement(pBsonElement);
        }
    }

    /* ------------------------- ExpressionAdd ----------------------------- */

    ExpressionAdd::~ExpressionAdd() {
    }

    intrusive_ptr<ExpressionNary> ExpressionAdd::create() {
        intrusive_ptr<ExpressionAdd> pExpression(new ExpressionAdd());
        return pExpression;
    }

    Value ExpressionAdd::evaluate(const Document& pDocument) const {

        /*
          We'll try to return the narrowest possible result value.  To do that
          without creating intermediate Values, do the arithmetic for double
          and integral types in parallel, tracking the current narrowest
          type.
         */
        double doubleTotal = 0;
        long long longTotal = 0;
        BSONType totalType = NumberInt;
        bool haveDate = false;

        const size_t n = vpOperand.size();
        for (size_t i = 0; i < n; ++i) {
            Value val = vpOperand[i]->evaluate(pDocument);

            if (val.numeric()) {
                totalType = Value::getWidestNumeric(totalType, val.getType());

                doubleTotal += val.coerceToDouble();
                longTotal += val.coerceToLong();
            }
            else if (val.getType() == Date) {
                uassert(16612, "only one Date allowed in an $add expression",
                        !haveDate);
                haveDate = true;

                // We don't manipulate totalType here.

                longTotal += val.getDate();
                doubleTotal += val.getDate();
            }
            else if (val.nullish()) {
                return Value(BSONNULL);
            }
            else {
                uasserted(16554, str::stream() << "$add only supports numeric or date types, not "
                                               << typeName(val.getType()));
            }
        }

        if (haveDate) {
            if (totalType == NumberDouble)
                longTotal = static_cast<long long>(doubleTotal);
            return Value(Date_t(longTotal));
        }
        else if (totalType == NumberLong) {
            return Value(longTotal);
        }
        else if (totalType == NumberDouble) {
            return Value(doubleTotal);
        }
        else if (totalType == NumberInt) {
            return Value::createIntOrLong(longTotal);
        }
        else {
            massert(16417, "$add resulted in a non-numeric type", false);
        }
    }

    const char *ExpressionAdd::getOpName() const {
        return "$add";
    }

    intrusive_ptr<ExpressionNary> (*ExpressionAdd::getFactory() const)() {
        return ExpressionAdd::create;
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
        // ExpressionNary::optimize() generates an ExpressionConstant for {$and:[]}.
        verify(n > 0);
        intrusive_ptr<Expression> pLast(pAnd->vpOperand[n - 1]);
        const ExpressionConstant *pConst =
            dynamic_cast<ExpressionConstant *>(pLast.get());
        if (!pConst)
            return pE;

        /*
          Evaluate and coerce the last argument to a boolean.  If it's false,
          then we can replace this entire expression.
         */
        bool last = pLast->evaluate(Document()).coerceToBool();
        if (!last) {
            intrusive_ptr<ExpressionConstant> pFinal(
                ExpressionConstant::create(Value(false)));
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

    Value ExpressionAnd::evaluate(const Document& pDocument) const {
        const size_t n = vpOperand.size();
        for(size_t i = 0; i < n; ++i) {
            Value pValue(vpOperand[i]->evaluate(pDocument));
            if (!pValue.coerceToBool())
                return Value(false);
        }

        return Value(true);
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

    void ExpressionCoerceToBool::addDependencies(set<string>& deps, vector<string>* path) const {
        pExpression->addDependencies(deps);
    }

    Value ExpressionCoerceToBool::evaluate(const Document& pDocument) const {
        Value pResult(pExpression->evaluate(pDocument));
        bool b = pResult.coerceToBool();
        if (b)
            return Value(true);
        return Value(false);
    }

    Value ExpressionCoerceToBool::serialize() const {
        MutableDocument valBuilder;
        // Serializing as an $and expression which will become a CoerceToBool
        vector<Value> arrayMaker;
        arrayMaker.push_back(pExpression->serialize());
        valBuilder["$and"] = Value(arrayMaker);
        return valBuilder.freezeToValue();
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
        /* GT  */ { { false, false, true },  Expression::LT,  "$gt"  },
        /* GTE */ { { false, true,  true },  Expression::LTE, "$gte" },
        /* LT  */ { { true,  false, false }, Expression::GT,  "$lt"  },
        /* LTE */ { { true,  true,  false }, Expression::GTE, "$lte" },
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
        // CMP and NE cannot use ExpressionFieldRange which is what this optimization uses
        if (newOp == CMP || newOp == NE)
            return pE;

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

    Value ExpressionCompare::evaluate(const Document& pDocument) const {
        checkArgCount(2);
        Value pLeft(vpOperand[0]->evaluate(pDocument));
        Value pRight(vpOperand[1]->evaluate(pDocument));

        int cmp = signum(Value::compare(pLeft, pRight));

        if (cmpOp == CMP) {
            switch(cmp) {
            case -1:
            case 0:
            case 1:
                return Value(cmp);

            default:
                verify(false); // CW TODO internal error
            }
        }

        bool returnValue = cmpLookup[cmpOp].truthValue[cmp + 1];
        return Value(returnValue);
    }

    const char *ExpressionCompare::getOpName() const {
        return cmpLookup[cmpOp].name;
    }

    /* ------------------------- ExpressionConcat ----------------------------- */

    ExpressionConcat::~ExpressionConcat() {
    }

    intrusive_ptr<ExpressionNary> ExpressionConcat::create() {
        return new ExpressionConcat();
    }

    Value ExpressionConcat::evaluate(const Document& input) const {
        const size_t n = vpOperand.size();

        StringBuilder result;
        for (size_t i = 0; i < n; ++i) {
            Value val = vpOperand[i]->evaluate(input);
            if (val.nullish())
                return Value(BSONNULL);

            uassert(16702, str::stream() << "$concat only supports strings, not "
                                         << typeName(val.getType()),
                    val.getType() == String);

            result << val.coerceToString();
        }

        return Value(result.str());
    }

    const char *ExpressionConcat::getOpName() const {
        return "$concat";
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

    Value ExpressionCond::evaluate(const Document& pDocument) const {
        checkArgCount(3);
        Value pCond(vpOperand[0]->evaluate(pDocument));
        int idx = pCond.coerceToBool() ? 1 : 2;
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
        pValue(Value(*pBsonElement)) {
    }

    intrusive_ptr<ExpressionConstant> ExpressionConstant::create(const Value& pValue) {
        intrusive_ptr<ExpressionConstant> pEC(new ExpressionConstant(pValue));
        return pEC;
    }

    ExpressionConstant::ExpressionConstant(const Value& pTheValue): pValue(pTheValue) {}


    intrusive_ptr<Expression> ExpressionConstant::optimize() {
        /* nothing to do */
        return intrusive_ptr<Expression>(this);
    }

    void ExpressionConstant::addDependencies(set<string>& deps, vector<string>* path) const {
        /* nothing to do */
    }

    Value ExpressionConstant::evaluate(const Document& pDocument) const {
        return pValue;
    }

    Value ExpressionConstant::serialize() const {
        return serializeConstant(pValue);
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

    Value ExpressionDayOfMonth::evaluate(const Document& pDocument) const {
        checkArgCount(1);
        Value pDate(vpOperand[0]->evaluate(pDocument));
        tm date = pDate.coerceToTm();
        return Value(date.tm_mday);
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

    Value ExpressionDayOfWeek::evaluate(const Document& pDocument) const {
        checkArgCount(1);
        Value pDate(vpOperand[0]->evaluate(pDocument));
        tm date = pDate.coerceToTm();
        return Value(date.tm_wday+1); // MySQL uses 1-7 tm uses 0-6
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

    Value ExpressionDayOfYear::evaluate(const Document& pDocument) const {
        checkArgCount(1);
        Value pDate(vpOperand[0]->evaluate(pDocument));
        tm date = pDate.coerceToTm();
        return Value(date.tm_yday+1); // MySQL uses 1-366 tm uses 0-365
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

    Value ExpressionDivide::evaluate(const Document& pDocument) const {
        checkArgCount(2);
        Value lhs = vpOperand[0]->evaluate(pDocument);
        Value rhs = vpOperand[1]->evaluate(pDocument);

        if (lhs.numeric() && rhs.numeric()) {
            double numer = lhs.coerceToDouble();
            double denom = rhs.coerceToDouble();
            uassert(16608, "can't $divide by zero",
                    denom != 0);

            return Value(numer / denom);
        }
        else if (lhs.nullish() || rhs.nullish()) {
            return Value(BSONNULL);
        }
        else {
            uasserted(16609, str::stream() << "$divide only supports numeric types, not "
                                           << typeName(lhs.getType())
                                           << " and "
                                           << typeName(rhs.getType()));
        }
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

    ExpressionObject::ExpressionObject(): _excludeId(false) {
    }

    intrusive_ptr<Expression> ExpressionObject::optimize() {
        for (ExpressionMap::iterator it(_expressions.begin()); it!=_expressions.end(); ++it) {
            if (it->second)
                it->second = it->second->optimize();
        }

        return intrusive_ptr<Expression>(this);
    }

    bool ExpressionObject::isSimple() {
        for (ExpressionMap::iterator it(_expressions.begin()); it!=_expressions.end(); ++it) {
            if (it->second && !it->second->isSimple())
                return false;
        }
        return true;
    }

    void ExpressionObject::addDependencies(set<string>& deps, vector<string>* path) const {
        string pathStr;
        if (path) {
            if (path->empty()) {
                // we are in the top level of a projection so _id is implicit
                if (!_excludeId)
                    deps.insert("_id");
            }
            else {
                FieldPath f (*path);
                pathStr = f.getPath(false);
                pathStr += '.';
            }
        }
        else {
            verify(!_excludeId);
        }
        

        for (ExpressionMap::const_iterator it(_expressions.begin()); it!=_expressions.end(); ++it) {
            if (it->second) {
                if (path) path->push_back(it->first);
                it->second->addDependencies(deps, path);
                if (path) path->pop_back();
            }
            else { // inclusion
                uassert(16407, "inclusion not supported in objects nested in $expressions",
                        path);

                deps.insert(pathStr + it->first);
            }
        }
    }

    void ExpressionObject::addToDocument(
        MutableDocument& out,
        const Document& pDocument,
        const Document& rootDoc
        ) const
    {
        const bool atRoot = (pDocument == rootDoc);

        ExpressionMap::const_iterator end = _expressions.end();

        // This is used to mark fields we've done so that we can add the ones we haven't
        set<string> doneFields;

        FieldIterator fields(pDocument);
        while(fields.more()) {
            Document::FieldPair field (fields.next());

            // TODO don't make a new string here
            const string fieldName = field.first.toString();
            ExpressionMap::const_iterator exprIter = _expressions.find(fieldName);

            // This field is not supposed to be in the output (unless it is _id)
            if (exprIter == end) {
                if (!_excludeId && atRoot && field.first == "_id") {
                    // _id from the root doc is always included (until exclusion is supported)
                    // not updating doneFields since "_id" isn't in _expressions
                    out.addField(field.first, field.second);
                }
                continue;
            }

            // make sure we don't add this field again
            doneFields.insert(exprIter->first);

            Expression* expr = exprIter->second.get();

            if (!expr) {
                // This means pull the matching field from the input document
                out.addField(field.first, field.second);
                continue;
            }

            ExpressionObject* exprObj = dynamic_cast<ExpressionObject*>(expr);
            BSONType valueType = field.second.getType();
            if ((valueType != Object && valueType != Array) || !exprObj ) {
                // This expression replace the whole field
                
                Value pValue(expr->evaluate(rootDoc));

                // don't add field if nothing was found in the subobject
                if (exprObj && pValue.getDocument()->getFieldCount() == 0)
                    continue;

                /*
                   Don't add non-existent values (note:  different from NULL or Undefined);
                   this is consistent with existing selection syntax which doesn't
                   force the appearance of non-existent fields.
                   */
                if (!pValue.missing())
                    out.addField(field.first, pValue);

                continue;
            }

            /*
                Check on the type of the input value.  If it's an
                object, just walk down into that recursively, and
                add it to the result.
            */
            if (valueType == Object) {
                MutableDocument sub (exprObj->getSizeHint());
                exprObj->addToDocument(sub, field.second.getDocument(), rootDoc);
                out.addField(field.first, sub.freezeToValue());
            }
            else if (valueType == Array) {
                /*
                    If it's an array, we have to do the same thing,
                    but to each array element.  Then, add the array
                    of results to the current document.
                */
                vector<Value> result;
                const vector<Value>& input = field.second.getArray();
                for (size_t i=0; i < input.size(); i++) {
                    // can't look for a subfield in a non-object value.
                    if (input[i].getType() != Object)
                        continue;

                    MutableDocument doc (exprObj->getSizeHint());
                    exprObj->addToDocument(doc, input[i].getDocument(), rootDoc);
                    result.push_back(doc.freezeToValue());
                }

                out.addField(field.first, Value(result));
            }
            else {
                verify( false );
            }
        }

        if (doneFields.size() == _expressions.size())
            return;

        /* add any remaining fields we haven't already taken care of */
        for (vector<string>::const_iterator i(_order.begin()); i!=_order.end(); ++i) {
            ExpressionMap::const_iterator it = _expressions.find(*i);
            string fieldName(it->first);

            /* if we've already dealt with this field, above, do nothing */
            if (doneFields.count(fieldName))
                continue;

            // this is a missing inclusion field
            if (!it->second)
                continue;

            Value pValue(it->second->evaluate(rootDoc));

            /*
              Don't add non-existent values (note:  different from NULL or Undefined);
              this is consistent with existing selection syntax which doesn't
              force the appearnance of non-existent fields.
            */
            if (pValue.missing())
                continue;

            // don't add field if nothing was found in the subobject
            if (dynamic_cast<ExpressionObject*>(it->second.get())
                    && pValue.getDocument()->getFieldCount() == 0)
                continue;


            out.addField(fieldName, pValue);
        }
    }

    size_t ExpressionObject::getSizeHint() const {
        // Note: this can overestimate, but that is better than underestimating
        return _expressions.size() + (_excludeId ? 0 : 1);
    }

    Document ExpressionObject::evaluateDocument(const Document& pDocument) const {
        /* create and populate the result */
        MutableDocument out (getSizeHint());
        
        addToDocument(out,
                      Document(), // No inclusion field matching.
                      pDocument);
        return out.freeze();
    }

    Value ExpressionObject::evaluate(const Document& pDocument) const {
        return Value(evaluateDocument(pDocument));
    }

    void ExpressionObject::addField(const FieldPath &fieldPath,
                                    const intrusive_ptr<Expression> &pExpression) {
        const string fieldPart = fieldPath.getFieldName(0);
        const bool haveExpr = _expressions.count(fieldPart);

        intrusive_ptr<Expression>& expr = _expressions[fieldPart]; // inserts if !haveExpr
        intrusive_ptr<ExpressionObject> subObj = dynamic_cast<ExpressionObject*>(expr.get());

        if (!haveExpr) {
            _order.push_back(fieldPart);
        }
        else { // we already have an expression or inclusion for this field
            if (fieldPath.getPathLength() == 1) {
                // This expression is for right here

                ExpressionObject* newSubObj = dynamic_cast<ExpressionObject*>(pExpression.get());
                uassert(16400, str::stream()
                             << "can't add an expression for field " << fieldPart
                             << " because there is already an expression for that field"
                             << " or one of its sub-fields.",
                        subObj && newSubObj); // we can merge them

                // Copy everything from the newSubObj to the existing subObj
                // This is for cases like { $project:{ 'b.c':1, b:{ a:1 } } }
                for (vector<string>::const_iterator it (newSubObj->_order.begin());
                                                    it != newSubObj->_order.end();
                                                    ++it) {
                    // asserts if any fields are dupes
                    subObj->addField(*it, newSubObj->_expressions[*it]);
                }
                return;
            }
            else {
                // This expression is for a subfield
                uassert(16401, str::stream()
                           << "can't add an expression for a subfield of " << fieldPart
                           << " because there is already an expression that applies to"
                           << " the whole field",
                        subObj);
            }
        }

        if (fieldPath.getPathLength() == 1) {
            verify(!haveExpr); // haveExpr case handled above.
            expr = pExpression;
            return;
        }

        if (!haveExpr)
            expr = subObj = ExpressionObject::create();

        subObj->addField(fieldPath.tail(), pExpression);
    }

    void ExpressionObject::includePath(const string &theFieldPath) {
        addField(theFieldPath, NULL);
    }

    Value ExpressionObject::serialize() const {
        MutableDocument valBuilder;
        if (_excludeId)
            valBuilder["_id"] = Value(false);

        for (vector<string>::const_iterator it(_order.begin()); it!=_order.end(); ++it) {
            string fieldName = *it;
            verify(_expressions.find(fieldName) != _expressions.end());
            intrusive_ptr<Expression> expr = _expressions.find(fieldName)->second;

            if (!expr) {
                // this is inclusion, not an expression
                valBuilder[fieldName] = Value(true);
            }
            else {
                valBuilder[fieldName] = expr->serialize();
            }
        }
        return valBuilder.freezeToValue();
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

    void ExpressionFieldPath::addDependencies(set<string>& deps, vector<string>* path) const {
        deps.insert(fieldPath.getPath(false));
    }

    Value ExpressionFieldPath::evaluatePathArray(size_t index, const Value& input) const {
        dassert(input.getType() == Array);

        // Check for remaining path in each element of array
        vector<Value> result;
        const vector<Value>& array = input.getArray();
        for (size_t i=0; i < array.size(); i++) {
            if (array[i].getType() != Object)
                continue;

            const Value nested = evaluatePath(index, array[i].getDocument());
            if (!nested.missing())
                result.push_back(nested);
        }

        return Value(result);
    }
    Value ExpressionFieldPath::evaluatePath(size_t index, const Document& input) const {
        // Note this function is very hot so it is important that is is well optimized.
        // In particular, all return paths should support RVO.

        /* if we've hit the end of the path, stop */
        if (index == fieldPath.getPathLength() - 1)
            return input[fieldPath.getFieldName(index)];

        // Try to dive deeper
        const Value val = input[fieldPath.getFieldName(index)];
        switch (val.getType()) {
        case Object:
            return evaluatePath(index+1, val.getDocument());

        case Array:
            return evaluatePathArray(index+1, val);

        default:
            return Value();
        }
    }

    Value ExpressionFieldPath::evaluate(const Document& pDocument) const {
        return evaluatePath(0, pDocument);
    }

    Value ExpressionFieldPath::serialize() const {
        return Value(fieldPath.getPath(true));
    }

    /* --------------------- ExpressionFieldRange -------------------------- */

    ExpressionFieldRange::~ExpressionFieldRange() {
    }

    intrusive_ptr<Expression> ExpressionFieldRange::optimize() {
        /* if there is no range to match, this will never evaluate true */
        if (!pRange.get())
            return ExpressionConstant::create(Value(false));

        /*
          If we ended up with a double un-ended range, anything matches.  I
          don't know how that can happen, given intersect()'s interface, but
          here it is, just in case.
        */
        if (pRange->pBottom.missing() && pRange->pTop.missing())
            return ExpressionConstant::create(Value(true));

        /*
          In all other cases, we have to test candidate values.  The
          intersect() method has already optimized those tests, so there
          aren't any more optimizations to look for here.
        */
        return intrusive_ptr<Expression>(this);
    }

    void ExpressionFieldRange::addDependencies(set<string>& deps, vector<string>* path) const {
        pFieldPath->addDependencies(deps);
    }

    Value ExpressionFieldRange::evaluate(const Document& pDocument) const {
        /* if there's no range, there can't be a match */
        if (!pRange.get())
            return Value(false);

        /* get the value of the specified field */
        Value pValue(pFieldPath->evaluate(pDocument));

        /* see if it fits within any of the ranges */
        if (pRange->contains(pValue))
            return Value(true);

        return Value(false);
    }

    Value ExpressionFieldRange::serialize() const {
        // serializing results in an unoptimized form that will be reoptimized at parse time

        if (!pRange.get()) {
            /* nothing will satisfy this predicate */
            return serializeConstant(Value(false));
        }

        if (pRange->pTop.missing() && pRange->pBottom.missing()) {
            /* any value will satisfy this predicate */
            return serializeConstant(Value(true));
        }

        // FIXME Append constant values using the $const operator.  SERVER-6769

        // FIXME This checks pointer equality not value equality.
        if (pRange->pTop == pRange->pBottom) {
            vector<Value> array;
            MutableDocument valBuilder;
            array.push_back(pFieldPath->serialize());
            array.push_back(serializeConstant(pRange->pTop));

            valBuilder["$eq"] = Value(array);
            return valBuilder.freezeToValue();
        }

        MutableDocument gtDoc;
        if (!pRange->pBottom.missing()) {
            vector<Value> array;
            array.push_back(pFieldPath->serialize());
            array.push_back(serializeConstant(pRange->pBottom));

            gtDoc[(pRange->bottomOpen ? "$gt" : "$gte")] = Value(array);

            if (pRange->pTop.missing()) {
                return gtDoc.freezeToValue();
            }
        }

        MutableDocument ltDoc;
        if (!pRange->pTop.missing()) {
            vector<Value> array;
            array.push_back(pFieldPath->serialize());
            array.push_back(serializeConstant(pRange->pTop));
            ltDoc[(pRange->topOpen ? "$lt" : "$lte")] = Value(array);

            if (pRange->pBottom.missing()) {
                return ltDoc.freezeToValue();
            }
        }

        vector<Value> array;
        MutableDocument valBuilder;
        array.push_back(gtDoc.freezeToValue());
        array.push_back(ltDoc.freezeToValue());
        valBuilder["$and"] = Value(array);

        return valBuilder.freezeToValue();
    }

    void ExpressionFieldRange::toMatcherBson(
        BSONObjBuilder *pBuilder) const {
        verify(pRange.get()); // otherwise, we can't do anything

        /* if there are no endpoints, then every value is accepted */
        if (pRange->pBottom.missing() && pRange->pTop.missing())
            return; // nothing to add to the predicate

        /* we're going to need the field path */
        string fieldPath(pFieldPath->getFieldPath(false));

        BSONObjBuilder range;
        if (!pRange->pBottom.missing()) {
            /* the test for equality doesn't generate a subobject */
            if (pRange->pBottom == pRange->pTop) {
                pRange->pBottom.addToBsonObj(pBuilder, fieldPath);
                return;
            }

            pRange->pBottom.addToBsonObj(
                pBuilder, (pRange->bottomOpen ? "$gt" : "$gte"));
        }

        if (!pRange->pTop.missing()) {
            pRange->pTop.addToBsonObj(
                pBuilder, (pRange->topOpen ? "$lt" : "$lte"));
        }

        pBuilder->append(fieldPath, range.done());
    }

    intrusive_ptr<ExpressionFieldRange> ExpressionFieldRange::create(
        const intrusive_ptr<ExpressionFieldPath> &pFieldPath, CmpOp cmpOp,
        const Value& pValue) {
        intrusive_ptr<ExpressionFieldRange> pE(
            new ExpressionFieldRange(pFieldPath, cmpOp, pValue));
        return pE;
    }

    ExpressionFieldRange::ExpressionFieldRange(
        const intrusive_ptr<ExpressionFieldPath> &pTheFieldPath, CmpOp cmpOp,
        const Value& pValue):
        pFieldPath(pTheFieldPath),
        pRange(new Range(cmpOp, pValue)) {
    }

    void ExpressionFieldRange::intersect(CmpOp cmpOp, const Value& pValue) {

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

    ExpressionFieldRange::Range::Range(CmpOp cmpOp, const Value& pValue):
        bottomOpen(false),
        topOpen(false),
        pBottom(),
        pTop() {
        switch(cmpOp) {

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

        case NE:
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
        const Value& pTheBottom, bool theBottomOpen,
        const Value& pTheTop, bool theTopOpen):
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
        Value pMaxBottom(pRange->pBottom);
        bool maxBottomOpen = pRange->bottomOpen;
        if (!pBottom.missing()) {
            if (pRange->pBottom.missing()) {
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
        Value pMinTop(pRange->pTop);
        bool minTopOpen = pRange->topOpen;
        if (!pTop.missing()) {
            if (pRange->pTop.missing()) {
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

    bool ExpressionFieldRange::Range::contains(const Value& pValue) const {
        if (!pBottom.missing()) {
            const int cmp = Value::compare(pValue, pBottom);
            if (cmp < 0)
                return false;
            if (bottomOpen && (cmp == 0))
                return false;
        }

        if (!pTop.missing()) {
            const int cmp = Value::compare(pValue, pTop);
            if (cmp > 0)
                return false;
            if (topOpen && (cmp == 0))
                return false;
        }

        return true;
    }

    /* ------------------------- ExpressionMillisecond ----------------------------- */

    ExpressionMillisecond::~ExpressionMillisecond() {
    }

    intrusive_ptr<ExpressionNary> ExpressionMillisecond::create() {
        intrusive_ptr<ExpressionMillisecond> pExpression(new ExpressionMillisecond());
        return pExpression;
    }

    ExpressionMillisecond::ExpressionMillisecond():
        ExpressionNary() {
    }

    void ExpressionMillisecond::addOperand(const intrusive_ptr<Expression>& pExpression) {
        checkArgLimit(1);
        ExpressionNary::addOperand(pExpression);
    }

    Value ExpressionMillisecond::evaluate(const Document& document) const {
        checkArgCount(1);
        Value date(vpOperand[0]->evaluate(document));
        const int ms = date.coerceToDate() % 1000LL;
        // adding 1000 since dates before 1970 would have negative ms
        return Value(ms >= 0 ? ms : 1000 + ms);
    }

    const char *ExpressionMillisecond::getOpName() const {
        return "$millisecond";
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

    Value ExpressionMinute::evaluate(const Document& pDocument) const {
        checkArgCount(1);
        Value pDate(vpOperand[0]->evaluate(pDocument));
        tm date = pDate.coerceToTm();
        return Value(date.tm_min);
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

    Value ExpressionMod::evaluate(const Document& pDocument) const {
        checkArgCount(2);
        Value lhs = vpOperand[0]->evaluate(pDocument);
        Value rhs = vpOperand[1]->evaluate(pDocument);

        BSONType leftType = lhs.getType();
        BSONType rightType = rhs.getType();

        if (lhs.numeric() && rhs.numeric()) {
            // ensure we aren't modding by 0
            double right = rhs.coerceToDouble();

            uassert(16610, "can't $mod by 0",
                    right != 0);

            if (leftType == NumberDouble
                || (rightType == NumberDouble && rhs.coerceToInt() != right)) {
                // the shell converts ints to doubles so if right is larger than int max or
                // if right truncates to something other than itself, it is a real double.
                // Integer-valued double case is handled below

                double left = lhs.coerceToDouble();
                return Value(fmod(left, right));
            }
            else if (leftType == NumberLong || rightType == NumberLong) {
                // if either is long, return long
                long long left = lhs.coerceToLong();
                long long rightLong = rhs.coerceToLong();
                return Value(left % rightLong);
            }

            // lastly they must both be ints, return int
            int left = lhs.coerceToInt();
            int rightInt = rhs.coerceToInt();
            return Value(left % rightInt);
        }
        else if (lhs.nullish() || rhs.nullish()) {
            return Value(BSONNULL);
        }
        else {
            uasserted(16611, str::stream() << "$mod only supports numeric types, not "
                                           << typeName(lhs.getType())
                                           << " and "
                                           << typeName(rhs.getType()));
        }
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

    Value ExpressionMonth::evaluate(const Document& pDocument) const {
        checkArgCount(1);
        Value pDate(vpOperand[0]->evaluate(pDocument));
        tm date = pDate.coerceToTm();
        return Value(date.tm_mon + 1); // MySQL uses 1-12 tm uses 0-11
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

    Value ExpressionMultiply::evaluate(const Document& pDocument) const {
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
            Value val = vpOperand[i]->evaluate(pDocument);

            if (val.numeric()) {
                productType = Value::getWidestNumeric(productType, val.getType());

                doubleProduct *= val.coerceToDouble();
                longProduct *= val.coerceToLong();
            }
            else if (val.nullish()) {
                return Value(BSONNULL);
            }
            else {
                uasserted(16555, str::stream() << "$multiply only supports numeric types, not "
                                               << typeName(val.getType()));
            }
        }

        if (productType == NumberDouble)
            return Value(doubleProduct);
        else if (productType == NumberLong)
            return Value(longProduct);
        else if (productType == NumberInt)
            return Value::createIntOrLong(longProduct);
        else
            massert(16418, "$multiply resulted in a non-numeric type", false);
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

    Value ExpressionHour::evaluate(const Document& pDocument) const {
        checkArgCount(1);
        Value pDate(vpOperand[0]->evaluate(pDocument));
        tm date = pDate.coerceToTm();
        return Value(date.tm_hour);
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

    Value ExpressionIfNull::evaluate(const Document& pDocument) const {
        checkArgCount(2);

        Value pLeft(vpOperand[0]->evaluate(pDocument));
        if (!pLeft.nullish())
            return pLeft;

        Value pRight(vpOperand[1]->evaluate(pDocument));
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
                if (pConst->getValue().getType() == String)
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
            Value pResult(evaluate(Document()));
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
            Value pResult(pConst->evaluate(Document()));
            pNew->addOperand(ExpressionConstant::create(pResult));
        }

        return pNew;
    }

    void ExpressionNary::addDependencies(set<string>& deps, vector<string>* path) const {
        for(ExpressionVector::const_iterator i(vpOperand.begin());
            i != vpOperand.end(); ++i) {
            (*i)->addDependencies(deps);
        }
    }

    void ExpressionNary::addOperand(
        const intrusive_ptr<Expression> &pExpression) {
        vpOperand.push_back(pExpression);
    }

    intrusive_ptr<ExpressionNary> (*ExpressionNary::getFactory() const)() {
        return NULL;
    }

    Value ExpressionNary::serialize() const {
        MutableDocument valBuilder;

        const size_t nOperand = vpOperand.size();
        vector<Value> array;
        /* build up the array */
        for(size_t i = 0; i < nOperand; i++)
            array.push_back(vpOperand[i]->serialize());

        valBuilder[getOpName()] = Value(array);
        return valBuilder.freezeToValue();
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

    Value ExpressionNot::evaluate(const Document& pDocument) const {
        checkArgCount(1);
        Value pOp(vpOperand[0]->evaluate(pDocument));

        bool b = pOp.coerceToBool();
        return Value(!b);
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

    Value ExpressionOr::evaluate(const Document& pDocument) const {
        const size_t n = vpOperand.size();
        for(size_t i = 0; i < n; ++i) {
            Value pValue(vpOperand[i]->evaluate(pDocument));
            if (pValue.coerceToBool())
                return Value(true);
        }

        return Value(false);
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

        /* if the result isn't a disjunction, we can't do anything */
        ExpressionOr *pOr = dynamic_cast<ExpressionOr *>(pE.get());
        if (!pOr)
            return pE;

        /*
          Check the last argument on the result; if it's not constant (as
          promised by ExpressionNary::optimize(),) then there's nothing
          we can do.
        */
        const size_t n = pOr->vpOperand.size();
        // ExpressionNary::optimize() generates an ExpressionConstant for {$or:[]}.
        verify(n > 0);
        intrusive_ptr<Expression> pLast(pOr->vpOperand[n - 1]);
        const ExpressionConstant *pConst =
            dynamic_cast<ExpressionConstant *>(pLast.get());
        if (!pConst)
            return pE;

        /*
          Evaluate and coerce the last argument to a boolean.  If it's true,
          then we can replace this entire expression.
         */
        bool last = pLast->evaluate(Document()).coerceToBool();
        if (last) {
            intrusive_ptr<ExpressionConstant> pFinal(
                ExpressionConstant::create(Value(true)));
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

    Value ExpressionSecond::evaluate(const Document& pDocument) const {
        checkArgCount(1);
        Value pDate(vpOperand[0]->evaluate(pDocument));
        tm date = pDate.coerceToTm();
        return Value(date.tm_sec);
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

    Value ExpressionStrcasecmp::evaluate(const Document& pDocument) const {
        checkArgCount(2);
        Value pString1(vpOperand[0]->evaluate(pDocument));
        Value pString2(vpOperand[1]->evaluate(pDocument));

        /* boost::iequals returns a bool not an int so strings must actually be allocated */
        string str1 = boost::to_upper_copy( pString1.coerceToString() );
        string str2 = boost::to_upper_copy( pString2.coerceToString() );
        int result = str1.compare(str2);

        if (result == 0)
            return Value(0);
        else if (result > 0)
            return Value(1);
        else
            return Value(-1);
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

    Value ExpressionSubstr::evaluate(const Document& pDocument) const {
        checkArgCount(3);
        Value pString(vpOperand[0]->evaluate(pDocument));
        Value pLower(vpOperand[1]->evaluate(pDocument));
        Value pLength(vpOperand[2]->evaluate(pDocument));

        string str = pString.coerceToString();
        uassert(16034, str::stream() << getOpName() <<
                ":  starting index must be a numeric type (is BSON type " <<
                typeName(pLower.getType()) << ")",
                (pLower.getType() == NumberInt 
                 || pLower.getType() == NumberLong 
                 || pLower.getType() == NumberDouble));
        uassert(16035, str::stream() << getOpName() <<
                ":  length must be a numeric type (is BSON type " <<
                typeName(pLength.getType() )<< ")",
                (pLength.getType() == NumberInt 
                 || pLength.getType() == NumberLong 
                 || pLength.getType() == NumberDouble));
        string::size_type lower = static_cast< string::size_type >( pLower.coerceToLong() );
        string::size_type length = static_cast< string::size_type >( pLength.coerceToLong() );
        if ( lower >= str.length() ) {
            // If lower > str.length() then string::substr() will throw out_of_range, so return an
            // empty string if lower is not a valid string index.
            return Value("");
        }
        return Value(str.substr(lower, length));
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

    Value ExpressionSubtract::evaluate(const Document& pDocument) const {
        checkArgCount(2);
        Value lhs = vpOperand[0]->evaluate(pDocument);
        Value rhs = vpOperand[1]->evaluate(pDocument);
            
        BSONType diffType = Value::getWidestNumeric(rhs.getType(), lhs.getType());

        if (diffType == NumberDouble) {
            double right = rhs.coerceToDouble();
            double left = lhs.coerceToDouble();
            return Value(left - right);
        } 
        else if (diffType == NumberLong) {
            long long right = rhs.coerceToLong();
            long long left = lhs.coerceToLong();
            return Value(left - right);
        }
        else if (diffType == NumberInt) {
            long long right = rhs.coerceToLong();
            long long left = lhs.coerceToLong();
            return Value::createIntOrLong(left - right);
        }
        else if (lhs.nullish() || rhs.nullish()) {
            return Value(BSONNULL);
        }
        else if (lhs.getType() == Date) {
            if (rhs.getType() == Date) {
                long long timeDelta = lhs.getDate() - rhs.getDate();
                return Value(timeDelta);
            }
            else if (rhs.numeric()) {
                long long millisSinceEpoch = lhs.getDate() - rhs.coerceToLong();
                return Value(Date_t(millisSinceEpoch));
            }
            else {
                uasserted(16613, str::stream() << "cant $subtract a "
                                               << typeName(rhs.getType())
                                               << " from a Date");
            }
        }
        else {
            uasserted(16556, str::stream() << "cant $subtract a"
                                           << typeName(rhs.getType())
                                           << " from a "
                                           << typeName(lhs.getType()));
        }
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

    Value ExpressionToLower::evaluate(const Document& pDocument) const {
        checkArgCount(1);
        Value pString(vpOperand[0]->evaluate(pDocument));
        string str = pString.coerceToString();
        boost::to_lower(str);
        return Value(str);
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

    Value ExpressionToUpper::evaluate(const Document& pDocument) const {
        checkArgCount(1);
        Value pString(vpOperand[0]->evaluate(pDocument));
        string str(pString.coerceToString());
        boost::to_upper(str);
        return Value(str);
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

    Value ExpressionWeek::evaluate(const Document& pDocument) const {
        checkArgCount(1);
        Value pDate(vpOperand[0]->evaluate(pDocument));
        tm date = pDate.coerceToTm();
        int dayOfWeek = date.tm_wday;
        int dayOfYear = date.tm_yday;
        int prevSundayDayOfYear = dayOfYear - dayOfWeek; // may be negative
        int nextSundayDayOfYear = prevSundayDayOfYear + 7; // must be positive

        // Return the zero based index of the week of the next sunday, equal to the one based index
        // of the week of the previous sunday, which is to be returned.
        int nextSundayWeek = nextSundayDayOfYear / 7;

        // Verify that the week calculation is consistent with strftime "%U".
        DEV{
            char buf[3];
            verify(strftime(buf,3,"%U",&date));
            verify(int(str::toUnsigned(buf))==nextSundayWeek);
        }

        return Value(nextSundayWeek);
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

    Value ExpressionYear::evaluate(const Document& pDocument) const {
        checkArgCount(1);
        Value pDate(vpOperand[0]->evaluate(pDocument));
        tm date = pDate.coerceToTm();
        return Value(date.tm_year + 1900); // tm_year is years since 1900
    }

    const char *ExpressionYear::getOpName() const {
        return "$year";
    }

}
