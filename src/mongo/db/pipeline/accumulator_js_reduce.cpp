/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/accumulator_js_reduce.h"
#include "mongo/db/pipeline/make_js_function.h"

namespace mongo {

REGISTER_ACCUMULATOR(_internalJsReduce, AccumulatorInternalJsReduce::parseInternalJsReduce);

std::pair<boost::intrusive_ptr<Expression>, Accumulator::Factory>
AccumulatorInternalJsReduce::parseInternalJsReduce(boost::intrusive_ptr<ExpressionContext> expCtx,
                                                   BSONElement elem,
                                                   VariablesParseState vps) {
    uassert(31326,
            str::stream() << kAccumulatorName << " requires a document argument, but found "
                          << elem.type(),
            elem.type() == BSONType::Object);
    BSONObj obj = elem.embeddedObject();

    std::string funcSource;
    boost::intrusive_ptr<Expression> dataExpr;

    for (auto&& element : obj) {
        if (element.fieldNameStringData() == "eval") {
            funcSource = parseReduceFunction(element);
        } else if (element.fieldNameStringData() == "data") {
            dataExpr = Expression::parseOperand(expCtx, element, vps);
        } else {
            uasserted(31243,
                      str::stream() << "Invalid argument specified to " << kAccumulatorName << ": "
                                    << element.toString());
        }
    }
    uassert(31245,
            str::stream() << kAccumulatorName
                          << " requires 'eval' argument, recieved input: " << obj.toString(false),
            !funcSource.empty());
    uassert(31349,
            str::stream() << kAccumulatorName
                          << " requires 'data' argument, recieved input: " << obj.toString(false),
            dataExpr);

    auto factory = [expCtx, funcSource = funcSource]() {
        return AccumulatorInternalJsReduce::create(expCtx, funcSource);
    };

    return {std::move(dataExpr), std::move(factory)};
}

std::string AccumulatorInternalJsReduce::parseReduceFunction(BSONElement func) {
    uassert(
        31244,
        str::stream() << kAccumulatorName
                      << " requires the 'eval' argument to be of type string, or code but found "
                      << func.type(),
        func.type() == BSONType::String || func.type() == BSONType::Code);
    return func._asCode();
}

void AccumulatorInternalJsReduce::processInternal(const Value& input, bool merging) {
    if (input.missing()) {
        return;
    }
    uassert(31242,
            str::stream() << kAccumulatorName << " requires a document argument, but found "
                          << input.getType(),
            input.getType() == BSONType::Object);
    Document data = input.getDocument();

    uassert(
        31251,
        str::stream() << kAccumulatorName
                      << " requires the 'data' argument to have a 'k' and 'v' field. Instead found"
                      << data.toString(),
        data.size() == 2ull && !data["k"].missing() && !data["v"].missing());

    _key = data["k"];

    _values.push_back(data["v"]);
    _memUsageBytes += data["v"].getApproximateSize();
}

Value AccumulatorInternalJsReduce::getValue(bool toBeMerged) {
    if (_values.size() < 1) {
        return Value{};
    }

    Value result;
    // Keep reducing until we have exactly one value.
    while (true) {
        BSONArrayBuilder bsonValues;
        size_t numLeft = _values.size();
        for (; numLeft > 0; numLeft--) {
            Value val = _values[numLeft - 1];

            // Do not insert if doing so would exceed the the maximum allowed BSONObj size.
            if (bsonValues.len() + _key.getApproximateSize() + val.getApproximateSize() >
                BSONObjMaxUserSize) {
                // If we have reached the threshold for maximum allowed BSONObj size and only have a
                // single value then no progress will be made on reduce. We must fail when this
                // scenario is encountered.
                size_t numNextReduce = _values.size() - numLeft;
                uassert(31392, "Value too large to reduce", numNextReduce > 1);
                break;
            }
            bsonValues << val;
        }

        auto expCtx = getExpressionContext();
        auto reduceFunc = makeJsFunc(expCtx, _funcSource.toString());

        // Function signature: reduce(key, values).
        BSONObj params = BSON_ARRAY(_key << bsonValues.arr());
        // For reduce, the key and values are both passed as 'params' so there's no need to set
        // 'this'.
        BSONObj thisObj;
        Value reduceResult =
            expCtx->getJsExecWithScope()->callFunction(reduceFunc, params, thisObj);
        if (numLeft == 0) {
            result = reduceResult;
            break;
        } else {
            // Remove all values which have been reduced.
            _values.resize(numLeft);
            // Include most recent result in the set of values to be reduced.
            _values.push_back(reduceResult);
        }
    }

    // If we're merging after this, wrap the value in the same format it was inserted in.
    if (toBeMerged) {
        MutableDocument output;
        output.addField("k", _key);
        output.addField("v", result);
        return Value(output.freeze());
    } else {
        return result;
    }
}

boost::intrusive_ptr<Accumulator> AccumulatorInternalJsReduce::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, StringData funcSource) {

    return make_intrusive<AccumulatorInternalJsReduce>(expCtx, funcSource);
}

void AccumulatorInternalJsReduce::reset() {
    _values.clear();
    _memUsageBytes = sizeof(*this);
    _key = Value{};
}

// Returns this accumulator serialized as a Value along with the reduce function.
Document AccumulatorInternalJsReduce::serialize(boost::intrusive_ptr<Expression> expression,
                                                bool explain) const {
    return DOC(
        getOpName() << DOC("data" << expression->serialize(explain) << "eval" << _funcSource));
}
}  // namespace mongo
