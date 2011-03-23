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
#include "ExpressionDivide.h"

#include "Value.h"

namespace mongo
{
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
}
