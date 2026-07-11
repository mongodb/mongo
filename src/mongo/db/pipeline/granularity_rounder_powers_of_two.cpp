// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/granularity_rounder.h"
#include "mongo/platform/bits.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
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
    uassert(11785400, "A granularity rounder can only round finite numbers", std::isfinite(number));
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
