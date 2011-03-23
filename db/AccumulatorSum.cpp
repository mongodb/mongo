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
#include "db/AccumulatorSum.h"

#include "db/Value.h"

namespace mongo
{
    shared_ptr<const Value> AccumulatorSum::evaluate(
	shared_ptr<Document> pDocument) const
    {
	assert(vpOperand.size() == 1);
	shared_ptr<const Value> prhs(vpOperand[0]->evaluate(pDocument));

	BSONType resultType = NumberInt;
	BSONType valueType = pValue->getType();
	BSONType rhsType = prhs->getType();
	if ((valueType == NumberLong) || (rhsType == NumberLong))
	    resultType = NumberLong;
	if ((valueType == NumberDouble) || (rhsType == NumberDouble))
	    resultType = NumberDouble;

	if (resultType == NumberInt)
	{
	    int result = pValue->getInt() + prhs->getInt();
	    pValue = Value::createInt(result);
	}
	else if (resultType == NumberLong)
	{
	    long long result = pValue->getLong() + prhs->getLong();
	    pValue = Value::createLong(result);
	}
	else /* (resultType == NumberDouble) */
	{
	    double result = pValue->getDouble() + prhs->getDouble();
	    pValue = Value::createDouble(result);
	}

	return pValue;
    }

    shared_ptr<Accumulator> AccumulatorSum::create()
    {
	shared_ptr<AccumulatorSum> pSummer(new AccumulatorSum());
	return pSummer;
    }

    AccumulatorSum::AccumulatorSum():
	Accumulator(Value::getZero())
    {
    }
}
