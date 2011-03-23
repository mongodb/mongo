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
#include "ExpressionAnd.h"

#include "Value.h"

namespace mongo
{
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
}
