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
#include "ExpressionNot.h"

#include "Value.h"

namespace mongo
{
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
}
