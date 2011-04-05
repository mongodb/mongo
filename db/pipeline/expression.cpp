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
#include "db/pipeline/document.h"
#include "db/pipeline/value.h"

namespace mongo {

    /* --------------------------- Expression ------------------------------ */

    void Expression::toMatcherBson(BSONObjBuilder *pBuilder) const {
	assert(false && "Expression::toMatcherBson()");
    }

    Expression::ObjectCtx::ObjectCtx(int theOptions):
        options(theOptions),
        raveledField() {
    }

    void Expression::ObjectCtx::ravel(string fieldName) {
        assert(ravelOk());
        assert(!ravelUsed());
        assert(fieldName.size());
        raveledField = fieldName;
    }

    bool Expression::ObjectCtx::documentOk() const {
        return ((options & DOCUMENT_OK) != 0);
    }

    shared_ptr<Expression> Expression::parseObject(
        BSONElement *pBsonElement, ObjectCtx *pCtx) {
        /*
          An object expression can take any of the following forms:

          f0: {f1: ..., f2: ..., f3: ...}
          f0: {$operator:[operand1, operand2, ...]}
          f0: {$ravel:"fieldpath"}

          We handle $ravel as a special case, because this is done by the
          projection source.  For any other expression, we hand over control to
          code that parses the expression and returns an expression.
        */

        shared_ptr<Expression> pExpression; // the result
        shared_ptr<ExpressionDocument> pExpressionDocument; // alt result
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

                if (strcmp(pFieldName, "$ravel") != 0) {
                    pExpression = parseExpression(pFieldName, &fieldElement);
                }
                else {
                    assert(pCtx->ravelOk());
                    // CW TODO error: it's not OK to ravel in this context

                    assert(!pCtx->ravelUsed());
                    // CW TODO error: this projection already has a ravel

                    assert(fieldElement.type() == String);
                    // CW TODO $ravel operand must be single field name

                    // TODO should we require leading "$document." here?
                    string fieldPath(fieldElement.String());
                    pExpression = ExpressionFieldPath::create(fieldPath);
                    pCtx->ravel(fieldPath);
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

                    pExpressionDocument = ExpressionDocument::create();
                    pExpression = pExpressionDocument;

                    /* this "object" is not an operator expression */
                    isOp = 0;
                    kind = NOTOPERATOR;
                }

                BSONType fieldType = fieldElement.type();
                string fieldName(pFieldName);
                if (fieldType == Object) {
                    /* it's a nested document */
                    shared_ptr<Expression> pNested(
                        parseObject(&fieldElement, &ObjectCtx(
                         (pCtx->documentOk() ? ObjectCtx::DOCUMENT_OK : 0))));
                    pExpressionDocument->addField(fieldName, pNested);
                }
                else if (fieldType == String) {
                    /* it's a renamed field */
                    shared_ptr<Expression> pPath(
                        ExpressionFieldPath::create(fieldElement.String()));
                    pExpressionDocument->addField(fieldName, pPath);
                }
                else if (fieldType == NumberDouble) {
                    /* it's an inclusion specification */
                    int inclusion = (int)fieldElement.Double();
                    assert(inclusion == 1);
                    // CW TODO error: only positive inclusions allowed here

                    shared_ptr<Expression> pPath(
                        ExpressionFieldPath::create(fieldName));
                    pExpressionDocument->addField(fieldName, pPath);
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
            shared_ptr<Expression> pOperand(
                Expression::parseObject(pBsonElement,
                            &ObjectCtx(ObjectCtx::DOCUMENT_OK)));
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
            if (value.compare(0, 10, "$document.") != 0)
                goto ExpectConstant;  // assume plain string constant

            /* if we got here, this is a field path expression */
            string fieldPath(value.substr(10));
            shared_ptr<Expression> pFieldExpr(
                ExpressionFieldPath::create(fieldPath));
            return pFieldExpr;
        }

        case Object: {
            shared_ptr<Expression> pSubExpression(
                Expression::parseObject(pBsonElement,
                            &ObjectCtx(ObjectCtx::DOCUMENT_OK)));
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
        shared_ptr<Document> pDocument) const {
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

    void ExpressionAdd::toBson(
	BSONObjBuilder *pBuilder, string name, bool docPrefix) const {
	operandsToBson(pBuilder, "$add", true);
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
        shared_ptr<Document> pDocument) const {
        const size_t n = vpOperand.size();
        for(size_t i = 0; i < n; ++i) {
            shared_ptr<const Value> pValue(vpOperand[i]->evaluate(pDocument));
            if (!pValue->coerceToBool())
                return Value::getFalse();
        }

        return Value::getTrue();
    }

    void ExpressionAnd::toBson(
	BSONObjBuilder *pBuilder, string name, bool docPrefix) const {
	operandsToBson(pBuilder, "$and", docPrefix);
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
	shared_ptr<Expression> pExpression) {
        shared_ptr<ExpressionCoerceToBool> pNew(
	    new ExpressionCoerceToBool(pExpression));
        return pNew;
    }

    ExpressionCoerceToBool::ExpressionCoerceToBool(
	shared_ptr<Expression> pTheExpression):
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
        shared_ptr<Document> pDocument) const {

	shared_ptr<const Value> pResult(pExpression->evaluate(pDocument));
        bool b = pResult->coerceToBool();
        if (b)
            return Value::getTrue();
        return Value::getFalse();
    }

    void ExpressionCoerceToBool::toBson(
	BSONObjBuilder *pBuilder, string name, bool docPrefix) const {
	/*
	  There is no equivalent of this; if we find we need this, we
	  need to take other steps.
	*/
	assert(false && "not possible");
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

    void ExpressionCompare::addOperand(shared_ptr<Expression> pExpression) {
        assert(vpOperand.size() < 2); // CW TODO user error
        ExpressionNary::addOperand(pExpression);
    }

    /*
      Lookup table for truth value returns
    */
    static const bool lookup[6][3] = {
        /*  -1      0      1   */
        /* EQ  */ { false, true,  false },
        /* NE  */ { true,  false, true  },
        /* GT  */ { false, false, true  },
        /* GTE */ { false, true,  true  },
        /* LT  */ { true,  false, false },
        /* LTE */ { true,  true,  false },
    };

    shared_ptr<const Value> ExpressionCompare::evaluate(
        shared_ptr<Document> pDocument) const {
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
            cmp = left.compare(right);

            if (cmp < 0)
                cmp = -1;
            else if (cmp > 0)
                cmp = 1;
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

        bool returnValue = lookup[cmpOp][cmp + 1];
        if (returnValue)
            return Value::getTrue();
        return Value::getFalse();
    }

    void ExpressionCompare::toBson(
	BSONObjBuilder *pBuilder, string name, bool docPrefix) const {
	const char *pName = NULL;
	switch(cmpOp) {
	case EQ:
	    pName = "$eq";
	    break;

	case NE:
	    pName = "$ne";
	    break;

	case GT:
	    pName = "$gt";
	    break;

	case GTE:
	    pName = "$gte";
	    break;

	case LT:
	    pName = "$lt";
	    break;

	case LTE:
	    pName = "$lte";
	    break;

	case CMP:
	    pName = "$cmp";
	    break;
	}

	operandsToBson(pBuilder, pName, true);
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
        shared_ptr<const Value> pValue) {
        shared_ptr<ExpressionConstant> pEC(new ExpressionConstant(pValue));
        return pEC;
    }

    ExpressionConstant::ExpressionConstant(shared_ptr<const Value> pTheValue):
        pValue(pTheValue) {
    }


    shared_ptr<Expression> ExpressionConstant::optimize() {
	/* nothing to do */
	return shared_from_this();
    }

    shared_ptr<const Value> ExpressionConstant::evaluate(
        shared_ptr<Document> pDocument) const {
        return pValue;
    }

    void ExpressionConstant::toBson(
	BSONObjBuilder *pBuilder, string name, bool docPrefix) const {
	pValue->addToBsonObj(pBuilder, name);
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

    void ExpressionDivide::addOperand(shared_ptr<Expression> pExpression) {
        assert(vpOperand.size() < 2); // CW TODO user error
        ExpressionNary::addOperand(pExpression);
    }

    shared_ptr<const Value> ExpressionDivide::evaluate(
        shared_ptr<Document> pDocument) const {
        assert(vpOperand.size() == 2); // CW TODO user error
        shared_ptr<const Value> pLeft(vpOperand[0]->evaluate(pDocument));
        shared_ptr<const Value> pRight(vpOperand[1]->evaluate(pDocument));

        BSONType leftType = pLeft->getType();
        BSONType rightType = pRight->getType();
        assert(leftType == rightType);
        // CW TODO at least for now.  later, handle automatic conversions

        double left;
        double right;
        switch(leftType) {
        case NumberDouble:
            left = pLeft->getDouble();
            right = pRight->getDouble();
            break;

        case NumberLong:
            left = pLeft->getLong();
            right = pRight->getLong();
            break;

        case NumberInt:
            left = pLeft->getInt();
            right = pRight->getInt();
            break;

        default:
            assert(false); // CW TODO unimplemented for now
            break;
        }

        return Value::createDouble(left / right);
    }

    void ExpressionDivide::toBson(
	BSONObjBuilder *pBuilder, string name, bool docPrefix) const {
	operandsToBson(pBuilder, "$divide", true);
    }

    /* ---------------------- ExpressionDocument --------------------------- */

    ExpressionDocument::~ExpressionDocument() {
    }

    shared_ptr<ExpressionDocument> ExpressionDocument::create() {
        shared_ptr<ExpressionDocument> pExpression(new ExpressionDocument());
        return pExpression;
    }

    ExpressionDocument::ExpressionDocument():
        vFieldName(),
        vpExpression() {
    }

    shared_ptr<Expression> ExpressionDocument::optimize() {
	const size_t n = vpExpression.size();
	for(size_t i = 0; i < n; ++i) {
	    shared_ptr<Expression> pE(vpExpression[i]->optimize());
	    vpExpression[i] = pE;
	}

	return shared_from_this();
    }

    shared_ptr<const Value> ExpressionDocument::evaluate(
        shared_ptr<Document> pDocument) const {
        const size_t n = vFieldName.size();
        shared_ptr<Document> pResult(Document::create(n));
        for(size_t i = 0; i < n; ++i) {
            pResult->addField(vFieldName[i],
                              vpExpression[i]->evaluate(pDocument));
        }

        shared_ptr<const Value> pValue(Value::createDocument(pResult));
        return pValue;
    }

    void ExpressionDocument::addField(string fieldName,
                                      shared_ptr<Expression> pExpression) {
        vFieldName.push_back(fieldName);
        vpExpression.push_back(pExpression);
    }

    void ExpressionDocument::toBson(
	BSONObjBuilder *pBuilder, string name, bool docPrefix) const {

	BSONObjBuilder builder;
	const size_t n = vFieldName.size();
	for(size_t i = 0; i < n; ++i)
	    vpExpression[i]->toBson(&builder, vFieldName[i], docPrefix);

	pBuilder->append(name, builder.done());
    }


    /* --------------------- ExpressionFieldPath --------------------------- */

    ExpressionFieldPath::~ExpressionFieldPath() {
    }

    shared_ptr<ExpressionFieldPath> ExpressionFieldPath::create(
        string fieldPath) {
        shared_ptr<ExpressionFieldPath> pExpression(
            new ExpressionFieldPath(fieldPath));
        return pExpression;
    }

    ExpressionFieldPath::ExpressionFieldPath(string theFieldPath):
        vFieldPath() {
        /*
          The field path could be using dot notation.
          Break the field path up by peeling off successive pieces.
        */
        size_t startpos = 0;
        while(true) {
            /* find the next dot */
            const size_t dotpos = theFieldPath.find('.', startpos);

            /* if there are no more dots, use the remainder of the string */
            if (dotpos == theFieldPath.npos) {
                vFieldPath.push_back(theFieldPath.substr(startpos, dotpos));
                break;
            }

            /* use the string up to the dot */
            const size_t length = dotpos - startpos;
            assert(length); // CW TODO user error: no zero-length field names
            vFieldPath.push_back(
                theFieldPath.substr(startpos, length));

            /* next time, search starting one spot after that */
            startpos = dotpos + 1;
        }
    }

    shared_ptr<Expression> ExpressionFieldPath::optimize() {
	/* nothing can be done for these */
	return shared_from_this();
    }

    shared_ptr<const Value> ExpressionFieldPath::evaluate(
        shared_ptr<Document> pDocument) const {
        shared_ptr<const Value> pValue;
        const size_t n = vFieldPath.size();
        size_t i = 0;
        while(true) {
            pValue = pDocument->getValue(vFieldPath[i]);

            /* if the field doesn't exist, quit with a null value */
            if (!pValue.get())
                return Value::getNull();

            /* if we've hit the end of the path, stop */
            ++i;
            if (i >= n)
                break;

            /*
              We're diving deeper.  If the value was null, return null.
            */
            BSONType type = pValue->getType();
            if (type == jstNULL)
                return Value::getNull();
            if (type != Object)
                assert(false); // CW TODO user error:  must be a document

            /* extract from the next level down */
            pDocument = pValue->getDocument();
        }

        return pValue;
    }

    void ExpressionFieldPath::toBson(
	BSONObjBuilder *pBuilder, string name, bool docPrefix) const {
	stringstream ss;
	if (docPrefix)
	    ss << "$document.";

	ss << vFieldPath[0];

	const size_t n = vFieldPath.size();
	for(size_t i = 1; i < n; ++i)
	    ss << "." << vFieldPath[i];

	pBuilder->append(name, ss.str());
    }

    /* --------------------- ExpressionFieldPath --------------------------- */

    ExpressionFieldRange::~ExpressionFieldRange() {
    }

    shared_ptr<Expression> ExpressionFieldRange::optimize() {
	assert(false && "unimplemented"); // CW TODO
	return shared_ptr<Expression>();
    }

    shared_ptr<const Value> ExpressionFieldRange::evaluate(
	shared_ptr<Document> pDocument) const {
	assert(false && "unimplemented"); // CW TODO
	return shared_ptr<const Value>();
    }

    void ExpressionFieldRange::toBson(
	BSONObjBuilder *pBuilder, string name, bool docPrefix) {
	assert(false && "unimplemented"); // CW TODO
    }

    void ExpressionFieldRange::toMatcherBson(
	BSONObjBuilder *pBuilder) const {
	assert(false && "unimplemented"); // CW TODO
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

    void ExpressionIfNull::addOperand(shared_ptr<Expression> pExpression) {
        assert(vpOperand.size() < 2); // CW TODO user error
        ExpressionNary::addOperand(pExpression);
    }

    shared_ptr<const Value> ExpressionIfNull::evaluate(
        shared_ptr<Document> pDocument) const {
        assert(vpOperand.size() == 2); // CW TODO user error
        shared_ptr<const Value> pLeft(vpOperand[0]->evaluate(pDocument));

        if (pLeft->getType() != jstNULL)
            return pLeft;

        shared_ptr<const Value> pRight(vpOperand[1]->evaluate(pDocument));
        return pRight;
    }

    void ExpressionIfNull::toBson(
	BSONObjBuilder *pBuilder, string name, bool docPrefix) const {
	operandsToBson(pBuilder, "$ifnull", docPrefix);
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
	    shared_ptr<const Value> pResult(evaluate(shared_ptr<Document>()));
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
        shared_ptr<Expression> pExpression) {
        vpOperand.push_back(pExpression);
    }

    shared_ptr<ExpressionNary> (*ExpressionNary::getFactory() const)() {
	return NULL;
    }

    void ExpressionNary::operandsToBson(
	BSONObjBuilder *pBuilder, string opName, bool docPrefix) const {
	const size_t nOperand = vpOperand.size();
	assert(nOperand > 0);
	if (nOperand == 1)
	    vpOperand[0]->toBson(pBuilder, opName, docPrefix);
	else {
	    /* build up the array */
	    BSONObjBuilder builder;
	    char indexBuffer[21];
                // biggest unsigned long long is 18,446,744,073,709,551,615

	    for(size_t i = 0; i < nOperand; ++i) {
		// snprintf(indexBuffer, sizeof(indexBuffer), "%lu", i);
                    // CW TODO llu?  What's a size_t in 64 builds?
                    // CW TODO Do we care about such large arrays?
		// CW TODO FEH: snprintf not available until C++0x-blah
		// but this should be ok given max buffer size above
		sprintf(indexBuffer, "%lu", i);
		vpOperand[i]->toBson(&builder, indexBuffer, docPrefix);
	    }

	    pBuilder->appendArray(opName, builder.done());
	}
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

    void ExpressionNot::addOperand(shared_ptr<Expression> pExpression) {
        assert(vpOperand.size() < 1); // CW TODO user error
        ExpressionNary::addOperand(pExpression);
    }

    shared_ptr<const Value> ExpressionNot::evaluate(
        shared_ptr<Document> pDocument) const {
        assert(vpOperand.size() == 1); // CW TODO user error
        shared_ptr<const Value> pOp(vpOperand[0]->evaluate(pDocument));

        bool b = pOp->coerceToBool();
        if (b)
            return Value::getFalse();
        return Value::getTrue();
    }

    void ExpressionNot::toBson(
	BSONObjBuilder *pBuilder, string name, bool docPrefix) const {
	vpOperand[0]->toBson(pBuilder, "$not", docPrefix);
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
        shared_ptr<Document> pDocument) const {
        const size_t n = vpOperand.size();
        for(size_t i = 0; i < n; ++i) {
            shared_ptr<const Value> pValue(vpOperand[i]->evaluate(pDocument));
            if (pValue->coerceToBool())
                return Value::getTrue();
        }

        return Value::getFalse();
    }

    void ExpressionOr::toBson(
	BSONObjBuilder *pBuilder, string name, bool docPrefix) const {
	operandsToBson(pBuilder, "$or", docPrefix);
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
}
