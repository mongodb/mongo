/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_WINDOW_FUNCTION(
    locf,
    mongo::window_function::ExpressionFromLeftUnboundedWindowFunction<AccumulatorLocf>::parse,
    AllowedWithApiStrict::kAlways);

AccumulatorLocf::AccumulatorLocf(ExpressionContext* const expCtx)
    : AccumulatorForWindowFunctions(expCtx) {
    _memUsageTracker.set(sizeof(*this) + _lastNonNull.getApproximateSize());
}

void AccumulatorLocf::processInternal(const Value& input, bool merging) {
    tassert(6050100, "$locf can't be merged", !merging);

    if (!input.nullish()) {
        _lastNonNull = input;
        _memUsageTracker.set(sizeof(*this) + _lastNonNull.getApproximateSize());
    }
}

Value AccumulatorLocf::getValue(bool toBeMerged) {
    tassert(6050102, "$locf can't be merged", !toBeMerged);
    return _lastNonNull;
}

void AccumulatorLocf::reset() {
    _lastNonNull = Value(BSONNULL);
    _memUsageTracker.set(sizeof(*this) + _lastNonNull.getApproximateSize());
}

}  // namespace mongo
