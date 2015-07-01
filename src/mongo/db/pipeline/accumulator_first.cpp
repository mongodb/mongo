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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_ACCUMULATOR(first, AccumulatorFirst::create);

const char* AccumulatorFirst::getOpName() const {
    return "$first";
}

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

AccumulatorFirst::AccumulatorFirst() : _haveFirst(false) {
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
}
