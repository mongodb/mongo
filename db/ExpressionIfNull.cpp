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
#include "ExpressionIfNull.h"

#include "Value.h"

namespace mongo
{
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
}
