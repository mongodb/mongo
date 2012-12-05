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

    Value AccumulatorSum::evaluate(const Document& pDocument) const {
        verify(vpOperand.size() == 1);
        Value prhs(vpOperand[0]->evaluate(pDocument));

        BSONType rhsType = prhs.getType();

        // do nothing with non numeric types
        if (!(rhsType == NumberInt || rhsType == NumberLong || rhsType == NumberDouble))
            return Value();

        // upgrade to the widest type required to hold the result
        totalType = Value::getWidestNumeric(totalType, rhsType);

        if (totalType == NumberInt || totalType == NumberLong) {
            long long v = prhs.coerceToLong();
            longTotal += v;
            doubleTotal += v;
        }
        else if (totalType == NumberDouble) {
            double v = prhs.coerceToDouble();
            doubleTotal += v;
        }
        else {
            // non numerics should have returned above so we should never get here
            verify(false);
        }

        count++;

        return Value();
    }

    intrusive_ptr<Accumulator> AccumulatorSum::create(
        const intrusive_ptr<ExpressionContext> &pCtx) {
        intrusive_ptr<AccumulatorSum> pSummer(new AccumulatorSum());
        return pSummer;
    }

    Value AccumulatorSum::getValue() const {
        if (totalType == NumberLong) {
            return Value::createLong(longTotal);
        }
        else if (totalType == NumberDouble) {
            return Value::createDouble(doubleTotal);
        }
        else if (totalType == NumberInt) {
            return Value::createIntOrLong(longTotal);
        }
        else {
            massert(16000, "$sum resulted in a non-numeric type", false);
        }
    }

    AccumulatorSum::AccumulatorSum():
        Accumulator(),
        totalType(NumberInt),
        longTotal(0),
        doubleTotal(0),
        count(0) {
    }

    const char *AccumulatorSum::getOpName() const {
        return "$sum";
    }
}
