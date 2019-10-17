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
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/javascript_execution.h"

namespace mongo {

REGISTER_ACCUMULATOR(_internalJsReduce,
                     genericParseSingleExpressionAccumulator<AccumulatorInternalJsReduce>);

void AccumulatorInternalJsReduce::processInternal(const Value& input, bool merging) {
    if (input.missing()) {
        return;
    }

    uassert(31242,
            str::stream() << kAccumulatorName << " requires a document argument, but found "
                          << input.getType(),
            input.getType() == BSONType::Object);

    Document accInput = input.getDocument();
    uassert(31243,
            str::stream() << kAccumulatorName << " requires both an 'eval' and 'data' argument",
            accInput.size() == 2ull && !accInput["eval"].missing() && !accInput["data"].missing());

    uassert(
        31244,
        str::stream() << kAccumulatorName
                      << " requires the 'eval' argument to be of type string, or code but found "
                      << accInput["eval"].getType(),
        accInput["eval"].getType() == BSONType::String ||
            accInput["eval"].getType() == BSONType::Code);

    uassert(31245,
            str::stream() << kAccumulatorName
                          << " requires the 'data' argument to be of type Object, but found "
                          << accInput["data"].getType(),
            accInput["data"].getType() == BSONType::Object);

    Document data = accInput["data"].getDocument();
    _funcSource = accInput["eval"].getType() == BSONType::String ? accInput["eval"].getString()
                                                                 : accInput["eval"].getCode();

    uassert(31251,
            str::stream() << kAccumulatorName
                          << " requires the 'data' argument to have a 'k' and 'v' field",
            data.size() == 2ull && !data["k"].missing() && !data["v"].missing());

    _key = data["k"];

    _values.push_back(data["v"]);
    _memUsageBytes += data["v"].getApproximateSize();
}

Value AccumulatorInternalJsReduce::getValue(bool toBeMerged) {
    uassert(31241,
            "Cannot run server-side javascript without the javascript engine enabled",
            getGlobalScriptEngine());

    auto val = [&]() {
        if (_values.size() < 1) {
            return Value{};
        }

        BSONArrayBuilder bsonValues;
        for (const auto& val : _values) {
            bsonValues << val;
        }

        auto expCtx = getExpressionContext();
        auto jsExec = expCtx->getJsExecWithScope();

        ScriptingFunction func = jsExec->getScope()->createFunction(_funcSource.c_str());

        uassert(31247, "The reduce function failed to parse in the javascript engine", func);

        // Function signature: reduce(key, values).
        BSONObj params = BSON_ARRAY(_key << bsonValues.arr());
        // For reduce, the key and values are both passed as 'params' so there's no need to set
        // 'this'.
        BSONObj thisObj;
        return jsExec->callFunction(func, params, thisObj);
    }();

    // If we're merging after this, wrap the value in the same format it was inserted in.
    if (toBeMerged) {
        MutableDocument doc;
        MutableDocument output;
        doc.addField("k", _key);
        doc.addField("v", val);

        output.addField("eval", Value(_funcSource));
        output.addField("data", Value(doc.freeze()));

        return Value(output.freeze());
    } else {
        return val;
    }
}

boost::intrusive_ptr<Accumulator> AccumulatorInternalJsReduce::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    uassert(ErrorCodes::BadValue,
            str::stream() << kAccumulatorName << " not allowed without enabling test commands.",
            getTestCommandsEnabled());

    return new AccumulatorInternalJsReduce(expCtx);
}

void AccumulatorInternalJsReduce::reset() {
    _values.clear();
    _memUsageBytes = sizeof(*this);
    _key = Value{};
}
}  // namespace mongo
