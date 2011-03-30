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

#include "db/pipeline/document.h"
#include "db/pipeline/value.h"

namespace mongo
{
/* ----------------------------- ExpressionAdd ----------------------------- */

    ExpressionAdd::~ExpressionAdd()
    {
    }

    shared_ptr<ExpressionNary> ExpressionAdd::create()
    {
	shared_ptr<ExpressionNary> pExpression(new ExpressionAdd());
	return pExpression;
    }

    ExpressionAdd::ExpressionAdd():
	ExpressionNary()
    {
    }

    shared_ptr<const Value> ExpressionAdd::evaluate(
	shared_ptr<Document> pDocument) const
    {
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
	for(size_t i = 0; i < n; ++i)
	{
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

/* ----------------------------- ExpressionAnd ----------------------------- */

    ExpressionAnd::~ExpressionAnd()
    {
    }

    shared_ptr<ExpressionNary> ExpressionAnd::create()
    {
	shared_ptr<ExpressionNary> pExpression(new ExpressionAnd());
	return pExpression;
    }

    ExpressionAnd::ExpressionAnd():
	ExpressionNary()
    {
    }

    shared_ptr<const Value> ExpressionAnd::evaluate(
	shared_ptr<Document> pDocument) const
    {
	const size_t n = vpOperand.size();
	for(size_t i = 0; i < n; ++i)
	{
	    shared_ptr<const Value> pValue(vpOperand[i]->evaluate(pDocument));
	    if (!pValue->coerceToBool())
		return Value::getFalse();
	}

	return Value::getTrue();
    }

/* --------------------------- ExpressionCompare --------------------------- */

    ExpressionCompare::~ExpressionCompare()
    {
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createEq()
    {
	shared_ptr<ExpressionCompare> pExpression(
	    new ExpressionCompare(EQ));
	return pExpression;
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createNe()
    {
	shared_ptr<ExpressionCompare> pExpression(
	    new ExpressionCompare(NE));
	return pExpression;
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createGt()
    {
	shared_ptr<ExpressionCompare> pExpression(
	    new ExpressionCompare(GT));
	return pExpression;
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createGte()
    {
	shared_ptr<ExpressionCompare> pExpression(
	    new ExpressionCompare(GTE));
	return pExpression;
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createLt()
    {
	shared_ptr<ExpressionCompare> pExpression(
	    new ExpressionCompare(LT));
	return pExpression;
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createLte()
    {
	shared_ptr<ExpressionCompare> pExpression(
	    new ExpressionCompare(LTE));
	return pExpression;
    }

    shared_ptr<ExpressionNary> ExpressionCompare::createCmp()
    {
	shared_ptr<ExpressionCompare> pExpression(
	    new ExpressionCompare(CMP));
	return pExpression;
    }

    ExpressionCompare::ExpressionCompare(RelOp theRelOp):
	ExpressionNary(),
	relop(theRelOp)
    {
    }

    void ExpressionCompare::addOperand(shared_ptr<Expression> pExpression)
    {
	assert(vpOperand.size() < 2); // CW TODO user error
	ExpressionNary::addOperand(pExpression);
    }

    /*
      Lookup table for truth value returns
    */
    static const bool lookup[6][3] =
    {            /*  -1      0      1   */
	/* EQ  */ { false, true,  false },
	/* NE  */ { true,  false, true  },
	/* GT  */ { false, false, true  },
	/* GTE */ { false, true,  true  },
	/* LT  */ { true,  false, false },
	/* LTE */ { true,  true,  false },
    };

    shared_ptr<const Value> ExpressionCompare::evaluate(
	shared_ptr<Document> pDocument) const
    {
	assert(vpOperand.size() == 2); // CW TODO user error
	shared_ptr<const Value> pLeft(vpOperand[0]->evaluate(pDocument));
	shared_ptr<const Value> pRight(vpOperand[1]->evaluate(pDocument));

	BSONType leftType = pLeft->getType();
	BSONType rightType = pRight->getType();
	assert(leftType == rightType);
	// CW TODO at least for now.  later, handle automatic conversions

	int cmp = 0;
	switch(leftType)
	{
	case NumberDouble:
	{
	    double left = pLeft->getDouble();
	    double right = pRight->getDouble();

	    if (left < right)
		cmp = -1;
	    else if (left > right)
		cmp = 1;
	    break;
	}

	case NumberInt:
	{
	    int left = pLeft->getInt();
	    int right = pRight->getInt();

	    if (left < right)
		cmp = -1;
	    else if (left > right)
		cmp = 1;
	    break;
	}

	case String:
	{
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

	if (relop == CMP)
	{
	    switch(cmp)
	    {
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

	bool returnValue = lookup[relop][cmp + 1];
	if (returnValue)
	    return Value::getTrue();
	return Value::getFalse();
    }

/* -------------------------- ExpressionConstant --------------------------- */

    ExpressionConstant::~ExpressionConstant()
    {
    }

    shared_ptr<ExpressionConstant> ExpressionConstant::createFromBsonElement(
	BSONElement *pBsonElement)
    {
	shared_ptr<ExpressionConstant> pEC(
	    new ExpressionConstant(pBsonElement));
	return pEC;
    }

    ExpressionConstant::ExpressionConstant(BSONElement *pBsonElement):
	pValue(Value::createFromBsonElement(pBsonElement))
    {
    }

    shared_ptr<const Value> ExpressionConstant::evaluate(
	shared_ptr<Document> pDocument) const
    {
	return pValue;
    }

/* --------------------------- ExpressionDivide ---------------------------- */

    ExpressionDivide::~ExpressionDivide()
    {
    }

    shared_ptr<ExpressionNary> ExpressionDivide::create()
    {
	shared_ptr<ExpressionDivide> pExpression(new ExpressionDivide());
	return pExpression;
    }

    ExpressionDivide::ExpressionDivide():
	ExpressionNary()
    {
    }

    void ExpressionDivide::addOperand(shared_ptr<Expression> pExpression)
    {
	assert(vpOperand.size() < 2); // CW TODO user error
	ExpressionNary::addOperand(pExpression);
    }

    shared_ptr<const Value> ExpressionDivide::evaluate(
	shared_ptr<Document> pDocument) const
    {
	assert(vpOperand.size() == 2); // CW TODO user error
	shared_ptr<const Value> pLeft(vpOperand[0]->evaluate(pDocument));
	shared_ptr<const Value> pRight(vpOperand[1]->evaluate(pDocument));

	BSONType leftType = pLeft->getType();
	BSONType rightType = pRight->getType();
	assert(leftType == rightType);
	// CW TODO at least for now.  later, handle automatic conversions

	double left;
	double right;
	switch(leftType)
	{
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

/* -------------------------- ExpressionDocument --------------------------- */

    ExpressionDocument::~ExpressionDocument()
    {
    }

    shared_ptr<ExpressionDocument> ExpressionDocument::create()
    {
	shared_ptr<ExpressionDocument> pExpression(new ExpressionDocument());
	return pExpression;
    }

    ExpressionDocument::ExpressionDocument():
	vFieldName(),
	vpExpression()
    {
    }

    shared_ptr<const Value> ExpressionDocument::evaluate(
	shared_ptr<Document> pDocument) const
    {
	const size_t n = vFieldName.size();
	shared_ptr<Document> pResult(Document::create(n));
	for(size_t i = 0; i < n; ++i)
	{
	    pResult->addField(vFieldName[i],
			      vpExpression[i]->evaluate(pDocument));
	}

	shared_ptr<const Value> pValue(Value::createDocument(pResult));
	return pValue;
    }
    
    void ExpressionDocument::addField(string fieldName,
				      shared_ptr<Expression> pExpression)
    {
	vFieldName.push_back(fieldName);
	vpExpression.push_back(pExpression);
    }

/* ------------------------- ExpressionFieldPath --------------------------- */

    ExpressionFieldPath::~ExpressionFieldPath()
    {
    }

    shared_ptr<ExpressionFieldPath> ExpressionFieldPath::create(
	string fieldPath)
    {
	shared_ptr<ExpressionFieldPath> pExpression(
	    new ExpressionFieldPath(fieldPath));
	return pExpression;
    }

    ExpressionFieldPath::ExpressionFieldPath(string theFieldPath):
	vFieldPath()
    {
	/*
	  The field path could be using dot notation.
	  Break the field path up by peeling off successive pieces.
	*/
	size_t startpos = 0;
	while(true)
	{
	    /* find the next dot */
	    const size_t dotpos = theFieldPath.find('.', startpos);

	    /* if there are no more dots, use the remainder of the string */
	    if (dotpos == theFieldPath.npos)
	    {
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

    shared_ptr<const Value> ExpressionFieldPath::evaluate(
	shared_ptr<Document> pDocument) const
    {
	shared_ptr<const Value> pValue;
	const size_t n = vFieldPath.size();
	size_t i = 0;
	while(true)
	{
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

/* --------------------------- ExpressionIfNull ---------------------------- */

    ExpressionIfNull::~ExpressionIfNull()
    {
    }

    shared_ptr<ExpressionNary> ExpressionIfNull::create()
    {
	shared_ptr<ExpressionIfNull> pExpression(new ExpressionIfNull());
	return pExpression;
    }

    ExpressionIfNull::ExpressionIfNull():
	ExpressionNary()
    {
    }

    void ExpressionIfNull::addOperand(shared_ptr<Expression> pExpression)
    {
	assert(vpOperand.size() < 2); // CW TODO user error
	ExpressionNary::addOperand(pExpression);
    }

    shared_ptr<const Value> ExpressionIfNull::evaluate(
	shared_ptr<Document> pDocument) const
    {
	assert(vpOperand.size() == 2); // CW TODO user error
	shared_ptr<const Value> pLeft(vpOperand[0]->evaluate(pDocument));

	if (pLeft->getType() != jstNULL)
	    return pLeft;

	shared_ptr<const Value> pRight(vpOperand[1]->evaluate(pDocument));
	return pRight;
    }

/* ---------------------------- ExpressionNary ----------------------------- */

    ExpressionNary::ExpressionNary():
	vpOperand()
    {
    }

    void ExpressionNary::addOperand(
	shared_ptr<Expression> pExpression)
    {
	vpOperand.push_back(pExpression);
    }

/* ----------------------------- ExpressionNot ----------------------------- */

    ExpressionNot::~ExpressionNot()
    {
    }

    shared_ptr<ExpressionNary> ExpressionNot::create()
    {
	shared_ptr<ExpressionNot> pExpression(new ExpressionNot());
	return pExpression;
    }

    ExpressionNot::ExpressionNot():
	ExpressionNary()
    {
    }

    void ExpressionNot::addOperand(shared_ptr<Expression> pExpression)
    {
	assert(vpOperand.size() < 1); // CW TODO user error
	ExpressionNary::addOperand(pExpression);
    }

    shared_ptr<const Value> ExpressionNot::evaluate(
	shared_ptr<Document> pDocument) const
    {
	assert(vpOperand.size() == 1); // CW TODO user error
	shared_ptr<const Value> pOp(vpOperand[0]->evaluate(pDocument));

	bool b = pOp->coerceToBool();
	if (b)
	    return Value::getFalse();
	return Value::getTrue();
    }

/* ------------------------------ ExpressionOr ----------------------------- */

    ExpressionOr::~ExpressionOr()
    {
    }

    shared_ptr<ExpressionNary> ExpressionOr::create()
    {
	shared_ptr<ExpressionNary> pExpression(new ExpressionOr());
	return pExpression;
    }

    ExpressionOr::ExpressionOr():
	ExpressionNary()
    {
    }

    shared_ptr<const Value> ExpressionOr::evaluate(
	shared_ptr<Document> pDocument) const
    {
	const size_t n = vpOperand.size();
	for(size_t i = 0; i < n; ++i)
	{
	    shared_ptr<const Value> pValue(vpOperand[i]->evaluate(pDocument));
	    if (pValue->coerceToBool())
		return Value::getTrue();
	}

	return Value::getFalse();
    }

}
