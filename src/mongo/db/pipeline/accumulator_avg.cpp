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

#include "mongo/pch.h"

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

namespace {
    const char subTotalName[] = "subTotal";
    const char countName[] = "count";
}

    void AccumulatorAvg::processInternal(const Value& input, bool merging) {
        if (!merging) {
            Super::processInternal(input, merging);
        }
        else {
            // We expect an object that contains both a subtotal and a count.
            // This is what getValue(true) produced below.
            verify(input.getType() == Object);

            Value subTotal = input[subTotalName];
            verify(!subTotal.missing());
            doubleTotal += subTotal.getDouble();
                
            Value subCount = input[countName];
            verify(!subCount.missing());
            count += subCount.getLong();
        }
    }

    intrusive_ptr<Accumulator> AccumulatorAvg::create() {
        return new AccumulatorAvg();
    }

    Value AccumulatorAvg::getValue(bool toBeMerged) const {
        if (!toBeMerged) {
            double avg = 0;
            if (count)
                avg = doubleTotal / static_cast<double>(count);

            return Value(avg);
        }
        else {
            MutableDocument out;
            out.addField(subTotalName, Value(doubleTotal));
            out.addField(countName, Value(count));

            return Value(out.freeze());
        }
    }

    AccumulatorAvg::AccumulatorAvg() {
        // This is a fixed size Accumulator so we never need to update this
        _memUsageBytes = sizeof(*this);
    }

    void AccumulatorAvg::reset() {
        // All state is in parent
        Super::reset();
    }


    const char *AccumulatorAvg::getOpName() const {
        return "$avg";
    }
}
