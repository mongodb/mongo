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
#include "accumulator.h"

#include "db/pipeline/value.h"

namespace mongo
{
    shared_ptr<const Value> AccumulatorSum::evaluate(
	shared_ptr<Document> pDocument) const
    {
	assert(vpOperand.size() == 1);
	shared_ptr<const Value> prhs(vpOperand[0]->evaluate(pDocument));

	/* upgrade to the widest type required to hold the result */
	BSONType rhsType = prhs->getType();
	if ((resultType == NumberLong) || (rhsType == NumberLong))
	    resultType = NumberLong;
	if ((resultType == NumberDouble) || (rhsType == NumberDouble))
	    resultType = NumberDouble;

	if (resultType == NumberInt)
	{
	    int v = prhs->getInt();
	    longResult += v;
	    doubleResult += v;
	}
	else if (resultType == NumberLong)
	{
	    long long v = prhs->getLong();
	    longResult += v;
	    doubleResult += v;
	}
	else /* (resultType == NumberDouble) */
	{
	    double v = prhs->getDouble();
	    doubleResult += v;
	}

	return Value::getZero();
    }

    shared_ptr<Accumulator> AccumulatorSum::create()
    {
	shared_ptr<AccumulatorSum> pSummer(new AccumulatorSum());
	return pSummer;
    }

    shared_ptr<const Value> AccumulatorSum::getValue() const
    {
	if (resultType == NumberInt)
	    return Value::createInt((int)longResult);
	if (resultType == NumberLong)
	    return Value::createLong(longResult);
	return Value::createDouble(doubleResult);
    }

    AccumulatorSum::AccumulatorSum():
	Accumulator(),
	resultType(NumberInt),
	longResult(0),
	doubleResult(0)
    {
    }
}
