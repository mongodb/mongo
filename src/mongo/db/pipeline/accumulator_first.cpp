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

    void AccumulatorFirst::processInternal(const Value& input, bool merging) {
        /* only remember the first value seen */
        if (!_haveFirst) {
            // can't use pValue.missing() since we want the first value even if missing
            _haveFirst = true;
            _first = input;
            _memUsageBytes = sizeof(*this) + input.getApproximateSize() - sizeof(Value);
        }
    }

    Value AccumulatorFirst::getValue(bool toBeMerged) const {
        return _first;
    }

    AccumulatorFirst::AccumulatorFirst()
        : _haveFirst(false)
    {
        _memUsageBytes = sizeof(*this);
    }

    void AccumulatorFirst::reset() {
        _haveFirst = false;
        _first = Value();
        _memUsageBytes = sizeof(*this);
    }


    intrusive_ptr<Accumulator> AccumulatorFirst::create() {
        return new AccumulatorFirst();
    }

    const char *AccumulatorFirst::getOpName() const {
        return "$first";
    }
}
