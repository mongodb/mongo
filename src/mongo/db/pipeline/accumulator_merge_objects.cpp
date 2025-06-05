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
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_helpers.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/* ------------------------- AccumulatorMergeObjects ----------------------------- */

template <>
Value ExpressionFromAccumulator<AccumulatorMergeObjects>::evaluate(const Document& root,
                                                                   Variables* variables) const {
    return evaluateAccumulator(*this, root, variables);
}

REGISTER_ACCUMULATOR(mergeObjects,
                     genericParseSingleExpressionAccumulator<AccumulatorMergeObjects>);
REGISTER_STABLE_EXPRESSION(mergeObjects, ExpressionFromAccumulator<AccumulatorMergeObjects>::parse);

AccumulatorMergeObjects::AccumulatorMergeObjects(ExpressionContext* const expCtx)
    : AccumulatorState(expCtx) {
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorMergeObjects::reset() {
    _memUsageTracker.set(sizeof(*this));
    _output.reset();
}

void AccumulatorMergeObjects::processInternal(const Value& input, bool merging) {
    if (input.nullish()) {
        return;
    }

    uassert(40400,
            str::stream() << "$mergeObjects requires object inputs, but input " << input.toString()
                          << " is of type " << typeName(input.getType()),
            (input.getType() == BSONType::object));

    FieldIterator iter = input.getDocument().fieldIterator();
    while (iter.more()) {
        Document::FieldPair pair = iter.next();
        // Ignore missing values only, null and undefined are still considered.
        if (pair.second.missing())
            continue;

        _output.setField(pair.first, std::move(pair.second));
    }
    _memUsageTracker.set(sizeof(*this));
}

Value AccumulatorMergeObjects::getValue(bool toBeMerged) {
    return _output.freezeToValue();
}
}  // namespace mongo
