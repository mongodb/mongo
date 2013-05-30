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

    void AccumulatorMinMax::processInternal(const Value& input) {
        // nullish values should have no impact on result
        if (!input.nullish()) {
            /* compare with the current value; swap if appropriate */
            int cmp = Value::compare(_val, input) * _sense;
            if (cmp > 0 || _val.missing()) // missing is lower than all other values
                _val = input;
        }
    }

    Value AccumulatorMinMax::getValue() const {
        return _val;
    }

    AccumulatorMinMax::AccumulatorMinMax(int theSense)
        :_sense(theSense)
    { verify((_sense == 1) || (_sense == -1)); }

    intrusive_ptr<Accumulator> AccumulatorMinMax::createMin(
        const intrusive_ptr<ExpressionContext> &pCtx) {
        intrusive_ptr<AccumulatorMinMax> pAccumulator(
            new AccumulatorMinMax(1));
        return pAccumulator;
    }

    intrusive_ptr<Accumulator> AccumulatorMinMax::createMax(
        const intrusive_ptr<ExpressionContext> &pCtx) {
        intrusive_ptr<AccumulatorMinMax> pAccumulator(
            new AccumulatorMinMax(-1));
        return pAccumulator;
    }

    const char *AccumulatorMinMax::getOpName() const {
        if (_sense == 1)
            return "$min";
        return "$max";
    }
}
