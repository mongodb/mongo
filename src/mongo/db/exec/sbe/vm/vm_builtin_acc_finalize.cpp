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

#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo {
namespace sbe {
namespace vm {
// This function is necessary because 'aggDoubleDoubleSum()' result is 'Array' type but we need
// to produce a scalar value out of it.
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinDoubleDoubleSumFinalize(
    ArityType arity) {
    auto [_, fieldTag, fieldValue] = getFromStack(0);
    auto arr = value::getArrayView(fieldValue);
    return aggDoubleDoubleSumFinalizeImpl(arr);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinDoubleDoublePartialSumFinalize(
    ArityType arity) {
    auto [_, fieldTag, fieldValue] = getFromStack(0);
    return builtinDoubleDoublePartialSumFinalizeImpl(fieldTag, fieldValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinDoubleDoublePartialSumFinalizeImpl(
    value::TypeTags fieldTag, value::Value fieldValue) {
    // For {$sum: 1}, we use aggSum instruction. In this case, the result type is guaranteed to be
    // either 'NumberInt32', 'NumberInt64', or 'NumberDouble'. We should transform the scalar result
    // into an array which is the over-the-wire data format from a shard to a merging side.
    if (fieldTag == value::TypeTags::NumberInt32 || fieldTag == value::TypeTags::NumberInt64 ||
        fieldTag == value::TypeTags::NumberDouble) {
        auto [tag, val] = value::makeNewArray();
        value::ValueGuard guard{tag, val};
        auto newArr = value::getArrayView(val);

        DoubleDoubleSummation res;
        BSONType resType = BSONType::numberInt;
        switch (fieldTag) {
            case value::TypeTags::NumberInt32:
                res.addInt(value::bitcastTo<int32_t>(fieldValue));
                break;
            case value::TypeTags::NumberInt64:
                res.addLong(value::bitcastTo<long long>(fieldValue));
                resType = BSONType::numberLong;
                break;
            case value::TypeTags::NumberDouble:
                res.addDouble(value::bitcastTo<double>(fieldValue));
                resType = BSONType::numberDouble;
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(6546500);
        }
        auto [sum, addend] = res.getDoubleDouble();

        // The merge-side expects that the first element is the BSON type, not internal slot type.
        newArr->push_back(value::TypeTags::NumberInt32,
                          value::bitcastFrom<int>(stdx::to_underlying(resType)));
        newArr->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(sum));
        newArr->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(addend));

        guard.reset();
        return {true, tag, val};
    }

    tassert(6546501, "The result slot must be an Array", fieldTag == value::TypeTags::Array);
    auto arr = value::getArrayView(fieldValue);
    tassert(6294000,
            str::stream() << "The result slot must have at least "
                          << AggSumValueElems::kMaxSizeOfArray - 1
                          << " elements but got: " << arr->size(),
            arr->size() >= AggSumValueElems::kMaxSizeOfArray - 1);

    auto [tag, val] = makeCopyArray(*arr);
    value::ValueGuard guard{tag, val};
    auto newArr = value::getArrayView(val);

    // Replaces the first element by the corresponding 'BSONType'.
    auto bsonType = [=]() -> int {
        switch (arr->getAt(AggSumValueElems::kNonDecimalTotalTag).first) {
            case value::TypeTags::NumberInt32:
                return static_cast<int>(BSONType::numberInt);
            case value::TypeTags::NumberInt64:
                return static_cast<int>(BSONType::numberLong);
            case value::TypeTags::NumberDouble:
                return static_cast<int>(BSONType::numberDouble);
            default:
                MONGO_UNREACHABLE_TASSERT(6294001);
                return 0;
        }
    }();
    // The merge-side expects that the first element is the BSON type, not internal slot type.
    newArr->setAt(AggSumValueElems::kNonDecimalTotalTag,
                  value::TypeTags::NumberInt32,
                  value::bitcastFrom<int>(bsonType));

    guard.reset();
    return {true, tag, val};
}  // ByteCode::builtinDoubleDoublePartialSumFinalize

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinStdDevPopFinalize(ArityType arity) {
    auto [_, fieldTag, fieldValue] = getFromStack(0);

    return aggStdDevFinalizeImpl(fieldValue, false /* isSamp */);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinStdDevSampFinalize(
    ArityType arity) {
    auto [_, fieldTag, fieldValue] = getFromStack(0);

    return aggStdDevFinalizeImpl(fieldValue, true /* isSamp */);
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
