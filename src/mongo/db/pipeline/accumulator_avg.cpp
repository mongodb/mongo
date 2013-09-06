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
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
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
