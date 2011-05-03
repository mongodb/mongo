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

namespace mongo {

    boost::shared_ptr<const Value> AccumulatorSum::evaluate(
        boost::shared_ptr<Document> pDocument) const {
        assert(vpOperand.size() == 1);
	boost::shared_ptr<const Value> prhs(vpOperand[0]->evaluate(pDocument));

        /* upgrade to the widest type required to hold the result */
        BSONType rhsType = prhs->getType();
        if ((totalType == NumberLong) || (rhsType == NumberLong))
            totalType = NumberLong;
        if ((totalType == NumberDouble) || (rhsType == NumberDouble))
            totalType = NumberDouble;

        if (totalType == NumberInt) {
            int v = prhs->getInt();
            longTotal += v;
            doubleTotal += v;
        }
        else if (totalType == NumberLong) {
            long long v = prhs->getLong();
            longTotal += v;
            doubleTotal += v;
        }
        else { /* (totalType == NumberDouble) */
            double v = prhs->getDouble();
            doubleTotal += v;
        }

        return Value::getZero();
    }

    boost::shared_ptr<Accumulator> AccumulatorSum::create(
	const intrusive_ptr<ExpressionContext> &pCtx) {
	boost::shared_ptr<AccumulatorSum> pSummer(new AccumulatorSum());
        return pSummer;
    }

    boost::shared_ptr<const Value> AccumulatorSum::getValue() const {
        if (totalType == NumberInt)
            return Value::createInt((int)longTotal);
        if (totalType == NumberLong)
            return Value::createLong(longTotal);
        return Value::createDouble(doubleTotal);
    }

    AccumulatorSum::AccumulatorSum():
        Accumulator(),
        totalType(NumberInt),
        longTotal(0),
        doubleTotal(0) {
    }

    const char *AccumulatorSum::getOpName() const {
	return "$sum";
    }
}
