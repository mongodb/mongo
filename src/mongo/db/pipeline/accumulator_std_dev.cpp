
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
#include "math.h"
#include "pch.h"
#include "accumulator.h"

#include "db/pipeline/document.h"
#include "db/pipeline/expression_context.h"
#include "db/pipeline/value.h"

namespace mongo {

namespace {
    const char diffName[]     = "diff";
    const char subTotalName[] = "subTotal";
    const char countName[]    = "count";
}

    void AccumulatorStdDev::processInternal( const Value& input, bool merging) {
        if (!merging) {
            Super::processInternal(input, merging);
        }
        else {
            // We expect an object that contains a subtotal, diff, and count
            verify(input.getType() == Object);

            Value subTotal = input[subTotalName];
            verify(!subTotal.missing());
            double sumA = doubleTotal;
            double sumB = subTotal.getDouble();
            doubleTotal += sumB;

            Value subCount = input[countName];
            verify(!subCount.missing());
            long long countA = count;
            long long countB = subCount.getLong();
            count += countB;

            Value partialDiff = input[diffName];
            verify(!partialDiff.missing());

            double meanA = 0;
            double meanB = 0;
            if (countA)
                meanA = sumA / static_cast<double>(countA);
            if (countB)
                meanB = sumB / static_cast<double>(countB);
            double delta = meanA - meanB;

            double weight = 0;
            if (countA + countB)
                weight = (countA * countB) / static_cast<double>(countA + countB);

            diff += partialDiff.getDouble() + delta * delta * weight;
        }
    }

    intrusive_ptr<Accumulator> AccumulatorStdDev::createStdDev() {
        return new AccumulatorStdDev(STDDEV);
    }

    intrusive_ptr<Accumulator> AccumulatorStdDev::createStdDevPop() {
        return new AccumulatorStdDev(STDDEV_POP);
    }

    intrusive_ptr<Accumulator> AccumulatorStdDev::createVar() {
        return new AccumulatorStdDev(VAR);
    }

    intrusive_ptr<Accumulator> AccumulatorStdDev::createVarPop() {
        return new AccumulatorStdDev(VAR_POP);
    }

    Value AccumulatorStdDev::getValue(bool toBeMerged) const {
        if (!toBeMerged) {
            double var    = 0;
            double stdDev = 0;
            if( count > 1 && (_op == STDDEV || _op == VAR)) {
                var = diff / static_cast<double>(count - 1);
            }
            else if (count > 0 && (_op == STDDEV_POP || _op == VAR_POP))
                var = diff / static_cast<double>(count);

            stdDev = sqrt(var);
            if( _op == STDDEV || _op == STDDEV_POP )
                return Value(stdDev);
            return Value(var);
        }
        else {
            MutableDocument out;
            out.addField(subTotalName, Value(doubleTotal));
            out.addField(countName, Value(count));
            out.addField(diffName, Value(diff));

            return Value(out.freeze());
        }
    }

    AccumulatorStdDev::AccumulatorStdDev(StdDevOp op)
        :_op(op)
    {
        // This is a fixed size Accumulator so we never need to update this
        _memUsageBytes = sizeof(*this);
    }

    void AccumulatorStdDev::reset() {
        Super::reset();
    }

    const char *AccumulatorStdDev::getOpName() const {
        if( _op == STDDEV )
            return "$stdDev";
        else if( _op == STDDEV_POP )
            return "$stdDevPop";
        else if( _op == VAR )
            return "$var";
        else
            return "$varPop";
    }
}
