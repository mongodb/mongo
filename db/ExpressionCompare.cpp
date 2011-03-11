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
#include "ExpressionCompare.h"

#include "Document.h"
#include "Field.h"

namespace mongo
{
    ExpressionCompare::~ExpressionCompare()
    {
    }

    shared_ptr<ExpressionCompare> ExpressionCompare::create(
	vector<shared_ptr<Expression>> operand, RelOp relop)
    {
	shared_ptr<ExpressionCompare> pExpression(
	    new ExpressionCompare(operand, relop));
	return pExpression;
    }

    ExpressionCompare::ExpressionCompare(
	vector<shared_ptr<Expression>> theOperand, RelOp theRelOp):
	relop(theRelOp),
	operand(theOperand)
    {
	assert(operand.size() == 2); // CW TODO user error otherwise
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

    shared_ptr<const Field> ExpressionCompare::evaluate(
	shared_ptr<Document> pDocument) const
    {
	shared_ptr<Expression> pLeft(operand[0]->evaluate(pDocument));
	shared_ptr<Expression> pRight(operand[1]->evaluate(pDocument));

	BSONType leftType = pLeft->getType();
	BSONType rightType = pRight->getType();
	assert(leftType == rightType);
	// CW TODO at least for now.  later, handle automatic conversions

	int cmp = 0;
	switch(leftType)
	{
	case NumericDouble:
	{
	    double left = pLeft->getDouble();
	    double right = pRight->getDouble();

	    if (left < right)
		cmp = -1;
	    else if (left > right)
		cmp = 1;
	    break;
	}

	case NumericInt:
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
	    const int cmp = left.compare(right);

	    if (cmp < 0)
		cmp = -1;
	    else if (cmp > 0)
		cmp = 1;
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
		return Field::getMinusOne();
	    case 0:
		return Field::getZero();
	    case 1:
		return Field::getOne();

	    default:
		assert(false); // CW TODO internal error
		return Field::getNull();
	    }
	}

	bool returnValue = lookup[relop][cmp + 1];
	if (returnValue)
	    return Field::getTrue();
	return Field::getFalse();
    }
}
