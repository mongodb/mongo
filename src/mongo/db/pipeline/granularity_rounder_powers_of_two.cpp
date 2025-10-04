/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/granularity_rounder.h"
#include "mongo/platform/bits.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <cmath>
#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::string;

REGISTER_GRANULARITY_ROUNDER(POWERSOF2, GranularityRounderPowersOfTwo::create);

intrusive_ptr<GranularityRounder> GranularityRounderPowersOfTwo::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new GranularityRounderPowersOfTwo(expCtx);
}

namespace {
void uassertNonNegativeNumber(Value value) {
    uassert(40265,
            str::stream() << "A granularity rounder can only round numeric values, but found type: "
                          << typeName(value.getType()),
            value.numeric());

    double number = value.coerceToDouble();
    uassert(40266, "A granularity rounder cannot round NaN", !std::isnan(number));
    uassert(40267, "A granularity rounder can only round non-negative numbers", number >= 0.0);
}
}  // namespace

Value GranularityRounderPowersOfTwo::roundUp(Value value) {
    uassertNonNegativeNumber(value);

    if (value.coerceToDouble() == 0.0) {
        return value;
    }

    Value exp;
    if (value.getType() == BSONType::numberDouble) {
        exp = Value(static_cast<int>(std::floor(std::log2(value.getDouble())) + 1.0));
    } else if (value.getType() == BSONType::numberDecimal) {
        Decimal128 input = value.getDecimal();
        exp = Value(Decimal128(static_cast<int>((std::floor(input.log2().toDouble()) + 1.0))));
    } else {
        long long number = value.getLong();

        // We can find the log_2 of 'number' by counting the number of leading zeros to find its
        // first bit set. This is safe to do because we are working with positive values.
        exp = Value(63 - countLeadingZeros64(number) + 1);
    }

    return ExpressionPow::create(getExpCtx(), Value(2), exp)
        ->evaluate(Document(), &getExpCtx()->variables);
}

Value GranularityRounderPowersOfTwo::roundDown(Value value) {
    uassertNonNegativeNumber(value);

    if (value.coerceToDouble() == 0.0) {
        return value;
    }

    Value exp;
    if (value.getType() == BSONType::numberDouble) {
        exp = Value(static_cast<int>(std::ceil(std::log2(value.getDouble())) - 1.0));
    } else if (value.getType() == BSONType::numberDecimal) {
        Decimal128 input = value.getDecimal();
        exp = Value(Decimal128(static_cast<int>((std::ceil(input.log2().toDouble()) - 1.0))));
    } else {
        long long number = value.getLong();

        int leadingZeros = countLeadingZeros64(number);
        int trailingZeros = countTrailingZeros64(number);

        if (leadingZeros + trailingZeros == 63) {
            // If number is a power of 2, then we need to subtract an extra 1 so we round down to
            // the next power of 2.
            exp = Value(63 - leadingZeros - 1);
        } else {
            exp = Value(63 - leadingZeros);
        }
    }

    return ExpressionPow::create(getExpCtx(), Value(2), exp)
        ->evaluate(Document(), &getExpCtx()->variables);
}

string GranularityRounderPowersOfTwo::getName() {
    return _name;
}
}  //  namespace mongo
