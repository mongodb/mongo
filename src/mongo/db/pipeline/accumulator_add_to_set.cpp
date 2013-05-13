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
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {
    void AccumulatorAddToSet::processInternal(const Value& input, bool merging) {
        if (!merging) {
            if (!input.missing()) {
                bool inserted = set.insert(input).second;
                if (inserted) {
                    _memUsageBytes += input.getApproximateSize();
                }
            }
        }
        else {
            // If we're merging, we need to take apart the arrays we
            // receive and put their elements into the array we are collecting.
            // If we didn't, then we'd get an array of arrays, with one array
            // from each merge source.
            verify(input.getType() == Array);
            
            const vector<Value>& array = input.getArray();
            for (size_t i=0; i < array.size(); i++) {
                bool inserted = set.insert(array[i]).second;
                if (inserted) {
                    _memUsageBytes += array[i].getApproximateSize();
                }
            }
        }
    }

    Value AccumulatorAddToSet::getValue(bool toBeMerged) const {
        vector<Value> valVec(set.begin(), set.end());
        return Value::consume(valVec);
    }

    AccumulatorAddToSet::AccumulatorAddToSet() {
        _memUsageBytes = sizeof(*this);
    }

    void AccumulatorAddToSet::reset() {
        SetType().swap(set);
        _memUsageBytes = sizeof(*this);
    }

    intrusive_ptr<Accumulator> AccumulatorAddToSet::create() {
        return new AccumulatorAddToSet();
    }

    const char *AccumulatorAddToSet::getOpName() const {
        return "$addToSet";
    }
}
