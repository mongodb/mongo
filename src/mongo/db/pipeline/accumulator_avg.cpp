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
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/platform/decimal128.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_ACCUMULATOR(avg, AccumulatorAvg::create);
REGISTER_EXPRESSION(avg, ExpressionFromAccumulator<AccumulatorAvg>::parse);

const char* AccumulatorAvg::getOpName() const {
    return "$avg";
}

namespace {
const char subTotalName[] = "subTotal";
const char subTotalErrorName[] = "subTotalError";  // Used for extra precision
const char countName[] = "count";
}  // namespace

void AccumulatorAvg::processInternal(const Value& input, bool merging) {
    if (merging) {
        // We expect an object that contains both a subtotal and a count. Additionally there may
        // be an error value, that allows for additional precision.
        // 'input' is what getValue(true) produced below.
        verify(input.getType() == Object);
        // We're recursively adding the subtotal to get the proper type treatment, but this only
        // increments the count by one, so adjust the count afterwards. Similarly for 'error'.
        processInternal(input[subTotalName], false);
        _count += input[countName].getLong() - 1;
        Value error = input[subTotalErrorName];
        if (!error.missing()) {
            processInternal(error, false);
            _count--;  // The error correction only adjusts the total, not the number of items.
        }
        return;
    }

    switch (input.getType()) {
        case NumberDecimal:
            _decimalTotal = _decimalTotal.add(input.getDecimal());
            _isDecimal = true;
            break;
        case NumberLong:
            // Avoid summation using double as that loses precision.
            _nonDecimalTotal.addLong(input.getLong());
            break;
        case NumberInt:
        case NumberDouble:
            _nonDecimalTotal.addDouble(input.getDouble());
            break;
        default:
            dassert(!input.numeric());
            return;
    }
    _count++;
}

intrusive_ptr<Accumulator> AccumulatorAvg::create() {
    return new AccumulatorAvg();
}

Decimal128 AccumulatorAvg::_getDecimalTotal() const {
    return _decimalTotal.add(_nonDecimalTotal.getDecimal());
}

Value AccumulatorAvg::getValue(bool toBeMerged) const {
    if (toBeMerged) {
        if (_isDecimal)
            return Value(Document{{subTotalName, _getDecimalTotal()}, {countName, _count}});

        double total, error;
        std::tie(total, error) = _nonDecimalTotal.getDoubleDouble();
        return Value(
            Document{{subTotalName, total}, {countName, _count}, {subTotalErrorName, error}});
    }

    if (_count == 0)
        return Value(BSONNULL);

    if (_isDecimal)
        return Value(_getDecimalTotal().divide(Decimal128(static_cast<int64_t>(_count))));

    return Value(_nonDecimalTotal.getDouble() / static_cast<double>(_count));
}

AccumulatorAvg::AccumulatorAvg() : _isDecimal(false), _count(0) {
    // This is a fixed size Accumulator so we never need to update this
    _memUsageBytes = sizeof(*this);
}

void AccumulatorAvg::reset() {
    _isDecimal = false;
    _nonDecimalTotal = {};
    _decimalTotal = {};
    _count = 0;
}
}
