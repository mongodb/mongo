/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/platform/basic.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_ACCUMULATOR(
    _internalConstructStats,
    genericParseSBEUnsupportedSingleExpressionAccumulator<AccumulatorInternalConstructStats>);

AccumulatorInternalConstructStats::AccumulatorInternalConstructStats(
    ExpressionContext* const expCtx)
    : AccumulatorState(expCtx) {
    _memUsageBytes = sizeof(*this);
}

intrusive_ptr<AccumulatorState> AccumulatorInternalConstructStats::create(
    ExpressionContext* const expCtx) {
    return new AccumulatorInternalConstructStats(expCtx);
}

void AccumulatorInternalConstructStats::processInternal(const Value& input, bool merging) {
    if (_key.empty()) {
        _key = input.getDocument()["key"].getString();
    }
    _memUsageBytes = sizeof(*this) + _output.getApproximateSize();
}

Value AccumulatorInternalConstructStats::getValue(bool toBeMerged) {
    _output.setNestedField(FieldPath(_key), Value(1));
    return _output.freezeToValue();
}

void AccumulatorInternalConstructStats::reset() {
    _memUsageBytes = sizeof(*this);
    _key = "";
    _output.reset();
}

}  // namespace mongo
