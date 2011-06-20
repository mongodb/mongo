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

namespace mongo {

    /* --------------------------- Expression ------------------------------ */

    void Expression::toMatcherBson(BSONObjBuilder *pBuilder) const {
	assert(false && "Expression::toMatcherBson()");
    }

    Expression::ObjectCtx::ObjectCtx(int theOptions):
        options(theOptions),
        unwindField() {
    }

    void Expression::ObjectCtx::unwind(string fieldName) {
        assert(unwindOk());
        assert(!unwindUsed());
        assert(fieldName.size());
        unwindField = fieldName;
    }

    bool Expression::ObjectCtx::documentOk() const {
        return ((options & DOCUMENT_OK) != 0);
    }

    const char Expression::unwindName[] = "$unwind";

    shared_ptr<Expression> Expression::parseObject(
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

        shared_ptr<Expression> pExpression; // the result
        shared_ptr<ExpressionObject> pExpressionObject; // alt result
        int isOp = -1; /* -1 -> unknown, 0 -> not an operator, 1 -> operator */
        enum { UNKNOWN, NOTOPERATOR, OPERATOR } kind = UNKNOWN;

        BSONObj obj(pBsonElement->Obj());
        BSONObjIterator iter(obj);
        for(size_t fieldCount = 0; iter.more(); ++fieldCount) {
            BSONElement fieldElement(iter.next());
            const char *pFieldName = fieldElement.fieldName();

            if (pFieldName[0] == '$') {
                assert(fieldCount == 0);
                // CW TODO error:  operator must be only field

                /* we've determined this "object" is an operator expression */
                isOp = 1;
                kind = OPERATOR;

                if (strcmp(pFieldName, unwindName) != 0) {
                    pExpression = parseExpression(pFieldName, &fieldElement);
                }
                else {
                    assert(pCtx->unwindOk());
                    // CW TODO error: it's not OK to unwind in this context

                    assert(!pCtx->unwindUsed());
                    // CW TODO error: this projection already has an unwind

                    assert(fieldElement.type() == String);
                    // CW TODO $unwind operand must be single field name

                    // TODO should we require the leading field prefix here?
                    string fieldPath(fieldElement.String());
                    pExpression = ExpressionFieldPath::create(fieldPath);
                    pCtx->unwind(fieldPath);
                }
            }
            else {
                assert(isOp != 1);
                assert(kind != OPERATOR);
                // CW TODO error: can't accompany an operator expression

                /* if it's our first time, create the document expression */
                if (!pExpression.get()) {
                    assert(pCtx->documentOk());
                    // CW TODO error: document not allowed in this context

                    pExpressionObject = ExpressionObject::create();
                    pExpression = pExpressionObject;

                    /* this "object" is not an operator expression */
                    isOp = 0;
                    kind = NOTOPERATOR;
                }

                BSONType fieldType = fieldElement.type();
                string fieldName(pFieldName);
                if (fieldType == Object) {
                    /* it's a nested document */
		    ObjectCtx oCtx(
			(pCtx->documentOk() ? ObjectCtx::DOCUMENT_OK : 0));
                    shared_ptr<Expression> pNested(
                        parseObject(&fieldElement, &oCtx));
                    pExpressionObject->addField(fieldName, pNested);
                }
                else if (fieldType == String) {
                    /* it's a renamed field */
                    shared_ptr<Expression> pPath(
                        ExpressionFieldPath::create(fieldElement.String()));
                    pExpressionObject->addField(fieldName, pPath);
                }
                else if (fieldType == NumberDouble) {
                    /* it's an inclusion specification */
                    double inclusion = fieldElement.Double();
		    if (inclusion == 0)
			pExpressionObject->excludePath(fieldName);
		    else if (inclusion == 1)
			pExpressionObject->includePath(fieldName);
		    else
			assert(false);
                    // CW TODO error: only 0 or 1 allowed here
                }
                else if (fieldType == Bool) {
		    bool inclusion = fieldElement.Bool();
		    if (!inclusion)
			pExpressionObject->excludePath(fieldName);
		    else
			pExpressionObject->includePath(fieldName);
		}
		else { /* nothing else is allowed */
                    assert(false); // CW TODO error
                }
            }
        }

        return pExpression;
    }


    struct OpDesc {
        const char *pName;
        shared_ptr<ExpressionNary> (*pFactory)(void);
    };

    static int OpDescCmp(const void *pL, const void *pR) {
        return strcmp(((const OpDesc *)pL)->pName, ((const OpDesc *)pR)->pName);
    }

    /*
      Keep these sorted alphabetically so we can bsearch() them using
      OpDescCmp() above.
    */
    static const OpDesc OpTable[] = {
        {"$add", ExpressionAdd::create},
        {"$and", ExpressionAnd::create},
        {"$cmp", ExpressionCompare::createCmp},
        {"$divide", ExpressionDivide::create},
        {"$eq", ExpressionCompare::createEq},
        {"$gt", ExpressionCompare::createGt},
        {"$gte", ExpressionCompare::createGte},
        {"$ifnull", ExpressionIfNull::create},
        {"$lt", ExpressionCompare::createLt},
        {"$lte", ExpressionCompare::createLte},
        {"$ne", ExpressionCompare::createNe},
        {"$not", ExpressionNot::create},
        {"$or", ExpressionOr::create},
    };

    static const size_t NOp = sizeof(OpTable)/sizeof(OpTable[0]);

    shared_ptr<Expression> Expression::parseExpression(
        const char *pOpName, BSONElement *pBsonElement) {
        /* look for the specified operator */
        OpDesc key;
        key.pName = pOpName;
        const OpDesc *pOp = (const OpDesc *)bsearch(
                                &key, OpTable, NOp, sizeof(OpDesc), OpDescCmp);

        assert(pOp); // CW TODO error: invalid operator

        /* make the expression node */
        shared_ptr<ExpressionNary> pExpression((*pOp->pFactory)());

        /* add the operands to the expression node */
        BSONType elementType = pBsonElement->type();
        if (elementType == Object) {
            /* the operator must be unary and accept an object argument */
            BSONObj objOperand(pBsonElement->Obj());
	    ObjectCtx oCtx(ObjectCtx::DOCUMENT_OK);
            shared_ptr<Expression> pOperand(
                Expression::parseObject(pBsonElement, &oCtx));
            pExpression->addOperand(pOperand);
        }
        else if (elementType == Array) {
            /* multiple operands - an n-ary operator */
            vector<BSONElement> bsonArray(pBsonElement->Array());
            const size_t n = bsonArray.size();
            for(size_t i = 0; i < n; ++i) {
                BSONElement *pBsonOperand = &bsonArray[i];
                shared_ptr<Expression> pOperand(
		    Expression::parseOperand(pBsonOperand));
                pExpression->addOperand(pOperand);
            }
        }
        else { /* assume it's an atomic operand */
            shared_ptr<Expression> pOperand(
		Expression::parseOperand(pBsonElement));
            pExpression->addOperand(pOperand);
        }

        return pExpression;
    }

    shared_ptr<Expression> Expression::parseOperand(BSONElement *pBsonElement) {
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
	    string fieldPath(value.substr(1));
            shared_ptr<Expression> pFieldExpr(
                ExpressionFieldPath::create(fieldPath));
            return pFieldExpr;
        }

        case Object: {
	    ObjectCtx oCtx(ObjectCtx::DOCUMENT_OK);
            shared_ptr<Expression> pSubExpression(
                Expression::parseObject(pBsonElement, &oCtx));
            return pSubExpression;
        }

        default:
	ExpectConstant: {
                shared_ptr<Expression> pOperand(
                    ExpressionConstant::createFromBsonElement(pBsonElement));
                return pOperand;
            }

        } // switch(type)

        /* NOTREACHED */
        assert(false);
        return shared_ptr<Expression>();
    }

    /* ------------------------- ExpressionAdd ----------------------------- */

    ExpressionAdd::~ExpressionAdd() {
    }

    shared_ptr<ExpressionNary> ExpressionAdd::create() {
        shared_ptr<ExpressionAdd> pExpression(new ExpressionAdd());
        return pExpression;
    }

    ExpressionAdd::ExpressionAdd():
        ExpressionNary() {
    }

    shared_ptr<const Value> ExpressionAdd::evaluate(
        const shared_ptr<Document> &pDocument) const {
        /*
          We'll try to return the narrowest possible result value.  To do that
          without creating intermediate Values, do the arithmetic for double
          and integral types in parallel, tracking the current narrowest
          type.
         */
        double doubleTotal = 0;
        long long longTotal = 0;
        BSONType totalType = NumberInt;

        const size_t n = vpOperand.size();
        for(size_t i = 0; i < n; ++i) {
            shared_ptr<const Value> pValue(vpOperand[i]->evaluate(pDocument));

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

    shared_ptr<ExpressionNary> (*ExpressionAdd::getFactory() const)() {
	return ExpressionAdd::create;
    }

    /* ------------------------- ExpressionAnd ----------------------------- */

    ExpressionAnd::~ExpressionAnd() {
    }

    shared_ptr<ExpressionNary> ExpressionAnd::create() {
        shared_ptr<ExpressionNary> pExpression(new ExpressionAnd());
        return pExpression;
    }

    ExpressionAnd::ExpressionAnd():
        ExpressionNary() {
    }

    shared_ptr<Expression> ExpressionAnd::optimize() {
	/* optimize the conjunction as much as possible */
	shared_ptr<Expression> pE(ExpressionNary::optimize());

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
	shared_ptr<Expression> pLast(pAnd->vpOperand[n - 1]);
	const ExpressionConstant *pConst =
	    dynamic_cast<ExpressionConstant *>(pLast.get());
	if (!pConst)
	    return pE;

	/*
	  Evaluate and coerce the last argument to a boolean.  If it's false,
	  then we can replace this entire expression.
	 */
	bool last = pLast->evaluate(shared_ptr<Document>())->coerceToBool();
	if (!last) {
	    shared_ptr<ExpressionConstant> pFinal(
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
	    shared_ptr<Expression> pFinal(
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

    shared_ptr<const Value> ExpressionAnd::evaluate(
        const shared_ptr<Document> &pDocument) const {
        const size_t n = vpOperand.size();
        for(size_t i = 0; i < n; ++i) {
            shared_ptr<const Value> pValue(vpOperand[i]->evaluate(pDocument));
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
	assert(false && "unimplemented");
    }

    shared_ptr<ExpressionNary> (*ExpressionAnd::getFactory() const)() {
	return ExpressionAnd::create;
    }

    /* -------------------- ExpressionCoerceToBool ------------------------- */

    ExpressionCoerceToBool::~ExpressionCoerceToBool() {
    }

    shared_ptr<ExpressionCoerceToBool> ExpressionCoerceToBool::create(
	const shared_ptr<Expression> &pExpression) {
        shared_ptr<ExpressionCoerceToBool> pNew(
	    new ExpressionCoerceToBool(pExpression));
        return pNew;
    }

    ExpressionCoerceToBool::ExpressionCoerceToBool(
	const shared_ptr<Expression> &pTheExpression):
        Expression(),
        pExpression(pTheExpression) {
    }

    shared_ptr<Expression> ExpressionCoerceToBool::optimize() {
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

	return shared_from_this();
    }

    shared_ptr<const Value> ExpressionCoerceToBool::evaluate(
        const shared_ptr<Document> &pDocument) const {

	shared_ptr<const Value> pResult(pExpression->evaluate(pDocument));
        bool b = pResult->coerceToBool();
        if (b)
            return Value::getTrue();
        return Value::getFalse();
    }

    void ExpressionCoerceToBool::addToBsonObj(
	BSONObjBuilder *pBuilder, string fieldName, bool fieldPrefix) const {
	assert(false && "not possible"); // no equivalent of this
    }

    void ExpressionCoerceToBool::addToBsonArray(
	BSONArrayBuilder *pBuilder, bool fieldPrefix) const {
	assert(false && "not possible"); // no equivalent of this
    }

    /* ----------------------- ExpressionCompare --------------------------- */

    ExpressionCompare::~ExpressionCompare() {
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createEq() {
        shared_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(EQ));
        return pExpression;
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createNe() {
        shared_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(NE));
        return pExpression;
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createGt() {
        shared_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(GT));
        return pExpression;
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createGte() {
        shared_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(GTE));
        return pExpression;
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createLt() {
        shared_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(LT));
        return pExpression;
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createLte() {
        shared_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(LTE));
        return pExpression;
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createCmp() {
        shared_ptr<ExpressionCompare> pExpression(
            new ExpressionCompare(CMP));
        return pExpression;
    }

    ExpressionCompare::ExpressionCompare(CmpOp theCmpOp):
        ExpressionNary(),
        cmpOp(theCmpOp) {
    }

    void ExpressionCompare::addOperand(
	const shared_ptr<Expression> &pExpression) {
        assert(vpOperand.size() < 2); // CW TODO user error
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

    shared_ptr<Expression> ExpressionCompare::optimize() {
	/* first optimize the comparison operands */
	shared_ptr<Expression> pE(ExpressionNary::optimize());

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
	shared_ptr<Expression> pLeft(pCmp->vpOperand[0]);
	shared_ptr<Expression> pRight(pCmp->vpOperand[1]);
	shared_ptr<ExpressionFieldPath> pFieldPath(
	    dynamic_pointer_cast<ExpressionFieldPath>(pLeft));
	shared_ptr<ExpressionConstant> pConstant;
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

    shared_ptr<const Value> ExpressionCompare::evaluate(
        const shared_ptr<Document> &pDocument) const {
        assert(vpOperand.size() == 2); // CW TODO user error
        shared_ptr<const Value> pLeft(vpOperand[0]->evaluate(pDocument));
        shared_ptr<const Value> pRight(vpOperand[1]->evaluate(pDocument));

        BSONType leftType = pLeft->getType();
        BSONType rightType = pRight->getType();
        assert(leftType == rightType);
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

        default:
            assert(false); // CW TODO unimplemented for now
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
                assert(false); // CW TODO internal error
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

    /* ---------------------- ExpressionConstant --------------------------- */

    ExpressionConstant::~ExpressionConstant() {
    }

    shared_ptr<ExpressionConstant> ExpressionConstant::createFromBsonElement(
        BSONElement *pBsonElement) {
        shared_ptr<ExpressionConstant> pEC(
            new ExpressionConstant(pBsonElement));
        return pEC;
    }

    ExpressionConstant::ExpressionConstant(BSONElement *pBsonElement):
        pValue(Value::createFromBsonElement(pBsonElement)) {
    }

    shared_ptr<ExpressionConstant> ExpressionConstant::create(
        const shared_ptr<const Value> &pValue) {
        shared_ptr<ExpressionConstant> pEC(new ExpressionConstant(pValue));
        return pEC;
    }

    ExpressionConstant::ExpressionConstant(
	const shared_ptr<const Value> &pTheValue):
        pValue(pTheValue) {
    }


    shared_ptr<Expression> ExpressionConstant::optimize() {
	/* nothing to do */
	return shared_from_this();
    }

    shared_ptr<const Value> ExpressionConstant::evaluate(
        const shared_ptr<Document> &pDocument) const {
        return pValue;
    }

    void ExpressionConstant::addToBsonObj(
	BSONObjBuilder *pBuilder, string fieldName, bool fieldPrefix) const {
	pValue->addToBsonObj(pBuilder, fieldName);
    }

    void ExpressionConstant::addToBsonArray(
	BSONArrayBuilder *pBuilder, bool fieldPrefix) const {
	pValue->addToBsonArray(pBuilder);
    }

    const char *ExpressionConstant::getOpName() const {
	assert(false); // this has no name
	return NULL;
    }

    /* ----------------------- ExpressionDivide ---------------------------- */

    ExpressionDivide::~ExpressionDivide() {
    }

    shared_ptr<ExpressionNary> ExpressionDivide::create() {
        shared_ptr<ExpressionDivide> pExpression(new ExpressionDivide());
        return pExpression;
    }

    ExpressionDivide::ExpressionDivide():
        ExpressionNary() {
    }

    void ExpressionDivide::addOperand(
	const shared_ptr<Expression> &pExpression) {
        assert(vpOperand.size() < 2); // CW TODO user error
        ExpressionNary::addOperand(pExpression);
    }

    shared_ptr<const Value> ExpressionDivide::evaluate(
        const shared_ptr<Document> &pDocument) const {
        assert(vpOperand.size() == 2); // CW TODO user error
        shared_ptr<const Value> pLeft(vpOperand[0]->evaluate(pDocument));
        shared_ptr<const Value> pRight(vpOperand[1]->evaluate(pDocument));

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

    shared_ptr<ExpressionObject> ExpressionObject::create() {
        shared_ptr<ExpressionObject> pExpression(new ExpressionObject());
        return pExpression;
    }

    ExpressionObject::ExpressionObject():
	excludePaths(false),
	path(),
        vFieldName(),
        vpExpression() {
    }

    shared_ptr<Expression> ExpressionObject::optimize() {
	const size_t n = vpExpression.size();
	for(size_t i = 0; i < n; ++i) {
	    shared_ptr<Expression> pE(vpExpression[i]->optimize());
	    vpExpression[i] = pE;
	}

	return shared_from_this();
    }

    void ExpressionObject::addToDocument(
	const shared_ptr<Document> &pResult,
        const shared_ptr<Document> &pDocument) const {
	const size_t pathSize = path.size();
	set<string>::iterator end(path.end());

	/*
	  Take care of inclusions or exclusions.  Note that _id is special,
	  that that it is always included, unless it is specifically excluded.
	  we use excludeId for that in case excludePaths if false, which means
	  to include paths.
	*/
	if (pathSize) {
	    auto_ptr<FieldIterator> pIter(pDocument->createFieldIterator());
	    if (excludePaths) {
		while(pIter->more()) {
		    pair<string, shared_ptr<const Value> > field(pIter->next());

		    /*
		      If the field in the document is not in the exclusion set,
		      add it to the result document.

		      Note that exclusions are only allowed on leaves, so we
		      can assume we don't have to descend recursively here.
		     */
		    if (path.find(field.first) != end)
			continue; // we found it, so don't add it

		    pResult->addField(field.first, field.second);
		}
	    }
	    else { /* !excludePaths */
		while(pIter->more()) {
		    pair<string, shared_ptr<const Value> > field(
			pIter->next());
		    /*
		      If the field in the document is in the inclusion set,
		      add it to the result document.  Or, if we're not
		      excluding _id, and it is _id, include it.

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
			assert(pChild);

			/*
			  Check on the type of the result object.  If it's an
			  object, just walk down into that recursively, and
			  add it to the result.
			*/
			BSONType valueType = field.second->getType();
			if (valueType == Object) {
			    shared_ptr<Document> pD(
				pChild->evaluateDocument(
				    field.second->getDocument()));
			    pResult->addField(field.first,
					      Value::createDocument(pD));
			}
			else if (valueType == Array) {
			    /*
			      If it's an array, we have to do the same thing,
			      but to each array element.  Then, add the array
			      of results to the current document.
			    */
			    vector<shared_ptr<const Value> > result;
			    shared_ptr<ValueIterator> pVI(
				field.second->getArray());
			    while(pVI->more()) {
				shared_ptr<Document> pD(
				    pChild->evaluateDocument(
					pVI->next()->getDocument()));
				result.push_back(Value::createDocument(pD));
			    }

			    pResult->addField(field.first,
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

	    shared_ptr<const Value> pValue(
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
	const shared_ptr<Document> &pDocument) const {
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

    shared_ptr<Document> ExpressionObject::evaluateDocument(
        const shared_ptr<Document> &pDocument) const {
	/* create and populate the result */
        shared_ptr<Document> pResult(
	    Document::create(getSizeHint(pDocument)));
	addToDocument(pResult, pDocument);
        return pResult;
    }

    shared_ptr<const Value> ExpressionObject::evaluate(
        const shared_ptr<Document> &pDocument) const {
	return Value::createDocument(evaluateDocument(pDocument));
    }

    void ExpressionObject::addField(const string &fieldName,
				    const shared_ptr<Expression> &pExpression) {
	/* must have an expression */
	assert(pExpression.get());

	/* parse the field path */
	FieldPath fieldPath(fieldName);
	assert(fieldPath.getPathLength() == 1); // CW TODO ERROR

	/* make sure it isn't a name we've included or excluded */
	set<string>::iterator ex(path.find(fieldName));
	assert(ex == path.end()); // CW TODO ERROR

	/* make sure it isn't a name we've already got */
	const size_t n = vFieldName.size();
	for(size_t i = 0; i < n; ++i) {
	    assert(fieldName.compare(vFieldName[i]) != 0); // CW TODO ERROR
	}

	vFieldName.push_back(fieldName);
	vpExpression.push_back(pExpression);
    }

    void ExpressionObject::includePath(
	const FieldPath *pPath, size_t pathi, size_t pathn, bool excludeLast) {

	/* get the current path field name */
	string fieldName(pPath->getFieldName(pathi));
	assert(fieldName.length()); // must be non-zero length

	const size_t pathCount = path.size();

	/* if this is the leaf-most object, stop */
	if (pathi == pathn - 1) {
	    /*
	      Make sure the exclusion configuration of this node matches
	      the requested result.  Or, that this is the first (determining)
	      specification.
	    */
	    assert((excludePaths == excludeLast) || !pathCount);
                                                             // CW TODO ERROR

	    excludePaths = excludeLast; // if (!pathCount), set this
	    path.insert(fieldName);
	    return;
	}

	/* this level had better be about inclusions */
	assert(!excludePaths); // CW TODO ERROR

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
	    assert(pChild);
	}
	else {
	    /*
	      If we get here, the intervening child isn't already there,
	      so create it.
	    */
	    shared_ptr<ExpressionObject> pSharedChild(
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

    shared_ptr<Expression> ExpressionObject::getField(
	const string &fieldName) const {
	const size_t n = vFieldName.size();
	for(size_t i = 0; i < n; ++i) {
	    if (fieldName.compare(vFieldName[i]) == 0)
		return vpExpression[i];
	}

	/* if we got here, we didn't find it */
	return shared_ptr<Expression>();
    }

    void ExpressionObject::emitPaths(
	BSONObjBuilder *pBuilder, vector<string> *pvPath) const {
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
	for(set<string>::iterator end(path.end()),
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

		pBuilder->append(ss.str(), !excludePaths);
	    }
	    else {
		/*
		  If we found a matching field description, then we need to
		  descend into the next level.
		*/
		Expression *pE = vpExpression[iField].get();
		ExpressionObject *pEO = dynamic_cast<ExpressionObject *>(pE);
		assert(pEO);

		/*
		  Add the current field name to the path being built up,
		  then go down into the next level.
		 */
		PathPusher pathPusher(pvPath, vFieldName[iField]);
		pEO->emitPaths(pBuilder, pvPath);
	    }
	}
    }

    void ExpressionObject::documentToBson(
	BSONObjBuilder *pBuilder, bool fieldPrefix,
	const string &unwindField) const {

	/* emit any inclusion/exclusion paths */
	vector<string> vPath;
	emitPaths(pBuilder, &vPath);

	/* then add any expressions */
	const size_t nField = vFieldName.size();
	const set<string>::iterator pathEnd(path.end());
	for(size_t iField = 0; iField < nField; ++iField) {
	    string fieldName(vFieldName[iField]);

	    /* if we already took care of this, don't repeat it */
	    if (path.find(fieldName) != pathEnd)
		continue;

	    if (unwindField.compare(fieldName) == 0) {
		BSONObjBuilder unwind;
		vpExpression[iField]->addToBsonObj(
		    &unwind, Expression::unwindName, false);
		pBuilder->append(vFieldName[iField], unwind.done());
		continue;
	    }

	    vpExpression[iField]->addToBsonObj(
		pBuilder, fieldName, fieldPrefix);
	}
    }

    void ExpressionObject::addToBsonObj(
	BSONObjBuilder *pBuilder, string fieldName, bool fieldPrefix) const {

	BSONObjBuilder objBuilder;
	documentToBson(&objBuilder, fieldPrefix, string());
	pBuilder->append(fieldName, objBuilder.done());
    }

    void ExpressionObject::addToBsonArray(
	BSONArrayBuilder *pBuilder, bool fieldPrefix) const {

	BSONObjBuilder objBuilder;
	documentToBson(&objBuilder, fieldPrefix, string());
	pBuilder->append(objBuilder.done());
    }

    /* --------------------- ExpressionFieldPath --------------------------- */

    ExpressionFieldPath::~ExpressionFieldPath() {
    }

    shared_ptr<ExpressionFieldPath> ExpressionFieldPath::create(
        const string &fieldPath) {
        shared_ptr<ExpressionFieldPath> pExpression(
            new ExpressionFieldPath(fieldPath));
        return pExpression;
    }

    ExpressionFieldPath::ExpressionFieldPath(
	const string &theFieldPath):
        fieldPath(theFieldPath) {
    }

    shared_ptr<Expression> ExpressionFieldPath::optimize() {
	/* nothing can be done for these */
	return shared_from_this();
    }

    shared_ptr<const Value> ExpressionFieldPath::evaluatePath(
	size_t index, const size_t pathLength,
	shared_ptr<Document> pDocument) const {
        shared_ptr<const Value> pValue; /* the return value */

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
	    vector<shared_ptr<const Value> > result;
	    shared_ptr<ValueIterator> pIter(pValue->getArray());
	    while(pIter->more()) {
		shared_ptr<const Value> pItem(pIter->next());
		BSONType iType = pItem->getType();
		if ((iType == Undefined) || (iType == jstNULL)) {
		    result.push_back(pItem);
		    continue;
		}

		assert(iType == Object); // CW TODO error, can't navigate this
		shared_ptr<const Value> itemResult(
		    evaluatePath(index, pathLength, pItem->getDocument()));
		result.push_back(itemResult);
	    }

	    return Value::createArray(result);
	}

	assert(false); // CW TODO user error:  must be a document
	return shared_ptr<const Value>();
    }

    shared_ptr<const Value> ExpressionFieldPath::evaluate(
        const shared_ptr<Document> &pDocument) const {
	return evaluatePath(0, fieldPath.getPathLength(), pDocument);
    }

    void ExpressionFieldPath::addToBsonObj(
	BSONObjBuilder *pBuilder, string fieldName, bool fieldPrefix) const {
	pBuilder->append(fieldName, fieldPath.getPath(fieldPrefix));
    }

    void ExpressionFieldPath::addToBsonArray(
	BSONArrayBuilder *pBuilder, bool fieldPrefix) const {
	pBuilder->append(getFieldPath(fieldPrefix));
    }

    /* --------------------- ExpressionFieldPath --------------------------- */

    ExpressionFieldRange::~ExpressionFieldRange() {
    }

    shared_ptr<Expression> ExpressionFieldRange::optimize() {
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
	return shared_from_this();
    }

    shared_ptr<const Value> ExpressionFieldRange::evaluate(
	const shared_ptr<Document> &pDocument) const {
	/* if there's no range, there can't be a match */
	if (!pRange.get())
	    return Value::getFalse();

	/* get the value of the specified field */
	shared_ptr<const Value> pValue(pFieldPath->evaluate(pDocument));

	/* see if it fits within any of the ranges */
	if (pRange->contains(pValue))
	    return Value::getTrue();

	return Value::getFalse();
    }

    void ExpressionFieldRange::addToBson(
	Builder *pBuilder, bool fieldPrefix) const {
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
	    pFieldPath->addToBsonArray(&operands, true);
	    pRange->pTop->addToBsonArray(&operands);
	    
	    BSONObjBuilder equals;
	    equals.append("$eq", operands.arr());
	    pBuilder->append(&equals);
	    return;
	}

	BSONObjBuilder leftOperator;
	if (pRange->pBottom.get()) {
	    BSONArrayBuilder leftOperands;
	    pFieldPath->addToBsonArray(&leftOperands, true);
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
	    pFieldPath->addToBsonArray(&rightOperands, true);
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
	BSONObjBuilder *pBuilder, string fieldName, bool fieldPrefix) const {
	BuilderObj builder(pBuilder, fieldName);
	addToBson(&builder, fieldPrefix);
    }

    void ExpressionFieldRange::addToBsonArray(
	BSONArrayBuilder *pBuilder, bool fieldPrefix) const {
	BuilderArray builder(pBuilder);
	addToBson(&builder, fieldPrefix);
    }

    void ExpressionFieldRange::toMatcherBson(
	BSONObjBuilder *pBuilder) const {
	assert(pRange.get()); // otherwise, we can't do anything

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

    shared_ptr<ExpressionFieldRange> ExpressionFieldRange::create(
	const shared_ptr<ExpressionFieldPath> &pFieldPath, CmpOp cmpOp,
	const shared_ptr<const Value> &pValue) {
	shared_ptr<ExpressionFieldRange> pE(
	    new ExpressionFieldRange(pFieldPath, cmpOp, pValue));
	return pE;
    }

    ExpressionFieldRange::ExpressionFieldRange(
	const shared_ptr<ExpressionFieldPath> &pTheFieldPath, CmpOp cmpOp,
	const shared_ptr<const Value> &pValue):
        pFieldPath(pTheFieldPath),
	pRange(new Range(cmpOp, pValue)) {
    }

    void ExpressionFieldRange::intersect(
	CmpOp cmpOp, const shared_ptr<const Value> &pValue) {

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
	CmpOp cmpOp, const shared_ptr<const Value> &pValue):
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
	    assert(false); // not allowed
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
	const shared_ptr<const Value> &pTheBottom, bool theBottomOpen,
	const shared_ptr<const Value> &pTheTop, bool theTopOpen):
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
	shared_ptr<const Value> pMaxBottom(pRange->pBottom);
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
	shared_ptr<const Value> pMinTop(pRange->pTop);
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
	const shared_ptr<const Value> &pValue) const {
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

    /* ----------------------- ExpressionIfNull ---------------------------- */

    ExpressionIfNull::~ExpressionIfNull() {
    }

    shared_ptr<ExpressionNary> ExpressionIfNull::create() {
        shared_ptr<ExpressionIfNull> pExpression(new ExpressionIfNull());
        return pExpression;
    }

    ExpressionIfNull::ExpressionIfNull():
        ExpressionNary() {
    }

    void ExpressionIfNull::addOperand(
	const shared_ptr<Expression> &pExpression) {
        assert(vpOperand.size() < 2); // CW TODO user error
        ExpressionNary::addOperand(pExpression);
    }

    shared_ptr<const Value> ExpressionIfNull::evaluate(
        const shared_ptr<Document> &pDocument) const {
        assert(vpOperand.size() == 2); // CW TODO user error
        shared_ptr<const Value> pLeft(vpOperand[0]->evaluate(pDocument));
	BSONType leftType = pLeft->getType();

        if ((leftType != Undefined) && (leftType != jstNULL))
            return pLeft;

        shared_ptr<const Value> pRight(vpOperand[1]->evaluate(pDocument));
        return pRight;
    }

    const char *ExpressionIfNull::getOpName() const {
	return "$ifnull";
    }

    /* ------------------------ ExpressionNary ----------------------------- */

    ExpressionNary::ExpressionNary():
        vpOperand() {
    }

    shared_ptr<Expression> ExpressionNary::optimize() {
	size_t nConst = 0; // count of constant operands
	const size_t n = vpOperand.size();
	for(size_t i = 0; i < n; ++i) {
	    shared_ptr<Expression> pNew(vpOperand[i]->optimize());

	    /* subsitute the optimized expression */
	    vpOperand[i] = pNew;

	    /* check to see if the result was a constant */
	    if (dynamic_cast<ExpressionConstant *>(pNew.get()))
		++nConst;
	}

	/*
	  If all the operands are constant, we can replace this expression
	  with a constant.  We can find the value by evaluating this
	  expression over a NULL Document because evaluating the
	  ExpressionConstant never refers to the argument Document.
	*/
	if (nConst == n) {
	    shared_ptr<const Value> pResult(
		evaluate(shared_ptr<Document>()));
	    shared_ptr<Expression> pReplacement(
		ExpressionConstant::create(pResult));
	    return pReplacement;
	}

	/*
	  If the operator isn't commutative or associative, there's nothing
	  more we can do.  We test that by seeing if we can get a factory;
	  if we can, we can use it to construct a temporary expression which
	  we'll evaluate to collapse as many constants as we can down to
	  a single one.
	 */
	shared_ptr<ExpressionNary> (*const pFactory)() = getFactory();
	if (!pFactory)
	    return shared_from_this();

	/*
	  Create a new Expression that will be the replacement for this one.
	  We actually create two:  one to hold constant expressions, and
	  one to hold non-constants.  Once we've got these, we evaluate
	  the constant expression to produce a single value, as above.
	  We then add this operand to the end of the non-constant expression,
	  and return that.
	 */
	shared_ptr<ExpressionNary> pNew((*pFactory)());
	shared_ptr<ExpressionNary> pConst((*pFactory)());
	for(size_t i = 0; i < n; ++i) {
	    shared_ptr<Expression> pE(vpOperand[i]);
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
		    shared_ptr<ExpressionNary> (*const pChildFactory)() =
			pNary->getFactory();
		    if (pChildFactory != pFactory)
			pNew->addOperand(pE);
		    else {
			/* same factory, so flatten */
			size_t nChild = pNary->vpOperand.size();
			for(size_t iChild = 0; iChild < nChild; ++iChild) {
			    shared_ptr<Expression> pCE(
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
	    shared_ptr<const Value> pResult(
		pConst->evaluate(shared_ptr<Document>()));
	    pNew->addOperand(ExpressionConstant::create(pResult));
	}

	return pNew;
    }

    void ExpressionNary::addOperand(
        const shared_ptr<Expression> &pExpression) {
        vpOperand.push_back(pExpression);
    }

    shared_ptr<ExpressionNary> (*ExpressionNary::getFactory() const)() {
	return NULL;
    }

    void ExpressionNary::toBson(
	BSONObjBuilder *pBuilder, const char *pOpName, bool fieldPrefix) const {
	const size_t nOperand = vpOperand.size();
	assert(nOperand > 0);
	if (nOperand == 1) {
	    vpOperand[0]->addToBsonObj(pBuilder, pOpName, fieldPrefix);
	    return;
	}

	/* build up the array */
	BSONArrayBuilder arrBuilder;
	for(size_t i = 0; i < nOperand; ++i)
	    vpOperand[i]->addToBsonArray(&arrBuilder, true);

	pBuilder->append(pOpName, arrBuilder.arr());
    }

    void ExpressionNary::addToBsonObj(
	BSONObjBuilder *pBuilder, string fieldName, bool fieldPrefix) const {
	BSONObjBuilder exprBuilder;
	toBson(&exprBuilder, getOpName(), fieldPrefix);
	pBuilder->append(fieldName, exprBuilder.done());
    }

    void ExpressionNary::addToBsonArray(
	BSONArrayBuilder *pBuilder, bool fieldPrefix) const {
	BSONObjBuilder exprBuilder;
	toBson(&exprBuilder, getOpName(), fieldPrefix);
	pBuilder->append(exprBuilder.done());
    }

    /* ------------------------- ExpressionNot ----------------------------- */

    ExpressionNot::~ExpressionNot() {
    }

    shared_ptr<ExpressionNary> ExpressionNot::create() {
        shared_ptr<ExpressionNot> pExpression(new ExpressionNot());
        return pExpression;
    }

    ExpressionNot::ExpressionNot():
        ExpressionNary() {
    }

    void ExpressionNot::addOperand(const shared_ptr<Expression> &pExpression) {
        assert(vpOperand.size() < 1); // CW TODO user error
        ExpressionNary::addOperand(pExpression);
    }

    shared_ptr<const Value> ExpressionNot::evaluate(
        const shared_ptr<Document> &pDocument) const {
        assert(vpOperand.size() == 1); // CW TODO user error
        shared_ptr<const Value> pOp(vpOperand[0]->evaluate(pDocument));

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

    shared_ptr<ExpressionNary> ExpressionOr::create() {
        shared_ptr<ExpressionNary> pExpression(new ExpressionOr());
        return pExpression;
    }

    ExpressionOr::ExpressionOr():
        ExpressionNary() {
    }

    shared_ptr<const Value> ExpressionOr::evaluate(
        const shared_ptr<Document> &pDocument) const {
        const size_t n = vpOperand.size();
        for(size_t i = 0; i < n; ++i) {
            shared_ptr<const Value> pValue(vpOperand[i]->evaluate(pDocument));
            if (pValue->coerceToBool())
                return Value::getTrue();
        }

        return Value::getFalse();
    }

    void ExpressionOr::toMatcherBson(BSONObjBuilder *pBuilder) const {
	BSONObjBuilder opArray;
	const size_t n = vpOperand.size();
	for(size_t i = 0; i < n; ++i)
	    vpOperand[i]->toMatcherBson(&opArray);

	pBuilder->append("$or", opArray.done());
    }

    shared_ptr<ExpressionNary> (*ExpressionOr::getFactory() const)() {
	return ExpressionOr::create;
    }

    shared_ptr<Expression> ExpressionOr::optimize() {
	/* optimize the disjunction as much as possible */
	shared_ptr<Expression> pE(ExpressionNary::optimize());

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
	shared_ptr<Expression> pLast(pOr->vpOperand[n - 1]);
	const ExpressionConstant *pConst =
	    dynamic_cast<ExpressionConstant *>(pLast.get());
	if (!pConst)
	    return pE;

	/*
	  Evaluate and coerce the last argument to a boolean.  If it's true,
	  then we can replace this entire expression.
	 */
	bool last = pLast->evaluate(shared_ptr<Document>())->coerceToBool();
	if (last) {
	    shared_ptr<ExpressionConstant> pFinal(
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
	    shared_ptr<Expression> pFinal(
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
}
