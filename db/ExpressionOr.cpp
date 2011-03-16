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
#include "ExpressionOr.h"

#include "Value.h"

namespace mongo
{
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
	    shared_ptr<const Value> pBool(Value::coerceToBoolean(pValue));
	    if (pBool->getBool())
		return Value::getTrue();
	}

	return Value::getFalse();
    }
}
