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

#include <cmath>
#include <limits>

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/summation.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_ACCUMULATOR(sum, AccumulatorSum::create);
REGISTER_EXPRESSION(sum, ExpressionFromAccumulator<AccumulatorSum>::parse);

const char* AccumulatorSum::getOpName() const {
    return "$sum";
}

namespace {
const char subTotalName[] = "subTotal";
const char subTotalErrorName[] = "subTotalError";  // Used for extra precision.
}  // namespace


void AccumulatorSum::processInternal(const Value& input, bool merging) {
    if (!input.numeric()) {
        if (merging && input.getType() == Object) {
            // Process merge document, see getValue() below.
            nonDecimalTotal.addDouble(
                input[subTotalName].getDouble());              // Sum without adjusting type.
            processInternal(input[subTotalErrorName], false);  // Sum adjusting for type of error.
        }
        return;
    }

    // Upgrade to the widest type required to hold the result.
    totalType = Value::getWidestNumeric(totalType, input.getType());
    switch (input.getType()) {
        case NumberInt:
        case NumberLong:
            nonDecimalTotal.addLong(input.coerceToLong());
            break;
        case NumberDouble:
            nonDecimalTotal.addDouble(input.getDouble());
            break;
        case NumberDecimal:
            decimalTotal = decimalTotal.add(input.coerceToDecimal());
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

intrusive_ptr<Accumulator> AccumulatorSum::create() {
    return new AccumulatorSum();
}

Value AccumulatorSum::getValue(bool toBeMerged) const {
    switch (totalType) {
        case NumberInt:
            if (nonDecimalTotal.fitsLong())
                return Value::createIntOrLong(nonDecimalTotal.getLong());
        // Fallthrough.
        case NumberLong:
            if (nonDecimalTotal.fitsLong())
                return Value(nonDecimalTotal.getLong());
            if (toBeMerged) {
                // The value was too large for a NumberLong, so output a document with two values
                // adding up to the desired total. Older MongoDB versions used to ignore signed
                // integer overflow and cause undefined behavior, that in practice resulted in
                // values that would wrap around modulo 2**64. Now an older mongos with a newer
                // mongod will yield an error that $sum resulted in a non-numeric type, which is
                // OK for this case. Output the error using the totalType, so in the future we can
                // determine the correct totalType for the sum. For the error to exceed 2**63,
                //  more than 2**53 integers would have to be summed, which is impossible.
                double total;
                double error;
                std::tie(total, error) = nonDecimalTotal.getDoubleDouble();
                long long llerror = static_cast<long long>(error);
                return Value(DOC(subTotalName << total << subTotalErrorName << llerror));
            }
            // Sum doesn't fit a NumberLong, so return a NumberDouble instead.
            return Value(nonDecimalTotal.getDouble());

        case NumberDouble:
            return Value(nonDecimalTotal.getDouble());
        case NumberDecimal: {
            double sum, error;
            std::tie(sum, error) = nonDecimalTotal.getDoubleDouble();
            Decimal128 total;  // zero
            if (sum != 0) {
                total = total.add(Decimal128(sum, Decimal128::kRoundTo34Digits));
                total = total.add(Decimal128(error, Decimal128::kRoundTo34Digits));
            }
            total = total.add(decimalTotal);
            return Value(total);
        }
        default:
            MONGO_UNREACHABLE;
    }
}

AccumulatorSum::AccumulatorSum() {
    // This is a fixed size Accumulator so we never need to update this.
    _memUsageBytes = sizeof(*this);
}

void AccumulatorSum::reset() {
    totalType = NumberInt;
    nonDecimalTotal = {};
    decimalTotal = {};
}
}  // namespace mongo
