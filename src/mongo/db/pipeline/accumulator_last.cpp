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
#include "mongo/db/pipeline/value.h"

namespace mongo {

    void AccumulatorLast::processInternal(const Value& input, bool merging) {
        /* always remember the last value seen */
        _last = input;
        _memUsageBytes = sizeof(*this) + _last.getApproximateSize() - sizeof(Value);
    }

    Value AccumulatorLast::getValue(bool toBeMerged) const {
        return _last;
    }

    AccumulatorLast::AccumulatorLast() {
        _memUsageBytes = sizeof(*this);
    }

    void AccumulatorLast::reset() {
        _memUsageBytes = sizeof(*this);
        _last = Value();
    }

    intrusive_ptr<Accumulator> AccumulatorLast::create() {
        return new AccumulatorLast();
    }

    const char *AccumulatorLast::getOpName() const {
        return "$last";
    }
}
