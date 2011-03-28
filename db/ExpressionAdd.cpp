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
#include "ExpressionAdd.h"

#include "Value.h"

namespace mongo
{
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
}
