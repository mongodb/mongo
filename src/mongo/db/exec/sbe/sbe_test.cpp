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

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {

TEST(SBEValues, Basic) {
    using namespace std::literals;
    {
        const auto [tag, val] = value::makeNewString("small"sv);
        ASSERT_EQUALS(tag, value::TypeTags::StringSmall);

        value::releaseValue(tag, val);
    }

    {
        const auto [tag, val] = value::makeNewString("not so small string"sv);
        ASSERT_EQUALS(tag, value::TypeTags::StringBig);

        value::releaseValue(tag, val);
    }
    {
        const auto [tag, val] = value::makeNewObject();
        auto obj = value::getObjectView(val);

        const auto [fieldTag, fieldVal] = value::makeNewString("not so small string"sv);
        obj->push_back("field"sv, fieldTag, fieldVal);

        ASSERT_EQUALS(obj->size(), 1);
        const auto [checkTag, checkVal] = obj->getField("field"sv);

        ASSERT_EQUALS(fieldTag, checkTag);
        ASSERT_EQUALS(fieldVal, checkVal);

        value::releaseValue(tag, val);
    }
    {
        const auto [tag, val] = value::makeNewArray();
        auto obj = value::getArrayView(val);

        const auto [fieldTag, fieldVal] = value::makeNewString("not so small string"sv);
        obj->push_back(fieldTag, fieldVal);

        ASSERT_EQUALS(obj->size(), 1);
        const auto [checkTag, checkVal] = obj->getAt(0);

        ASSERT_EQUALS(fieldTag, checkTag);
        ASSERT_EQUALS(fieldVal, checkVal);

        value::releaseValue(tag, val);
    }
}

TEST(SBEValues, Hash) {
    auto tagInt32 = value::TypeTags::NumberInt32;
    auto valInt32 = value::bitcastFrom<int32_t>(-5);

    auto tagInt64 = value::TypeTags::NumberInt64;
    auto valInt64 = value::bitcastFrom<int64_t>(-5);

    auto tagDouble = value::TypeTags::NumberDouble;
    auto valDouble = value::bitcastFrom<double>(-5.0);

    auto [tagDecimal, valDecimal] = value::makeCopyDecimal(mongo::Decimal128(-5.0));

    ASSERT_EQUALS(value::hashValue(tagInt32, valInt32), value::hashValue(tagInt64, valInt64));
    ASSERT_EQUALS(value::hashValue(tagInt32, valInt32), value::hashValue(tagDouble, valDouble));
    ASSERT_EQUALS(value::hashValue(tagInt32, valInt32), value::hashValue(tagDecimal, valDecimal));

    value::releaseValue(tagDecimal, valDecimal);

    auto tagDoubleInf = value::TypeTags::NumberDouble;
    auto valDoubleInf = value::bitcastFrom<double>(std::numeric_limits<double>::infinity());

    auto [tagDecimalInf, valDecimalInf] =
        value::makeCopyDecimal(mongo::Decimal128(std::numeric_limits<double>::infinity()));

    ASSERT_EQUALS(value::hashValue(tagDoubleInf, valDoubleInf),
                  value::hashValue(tagDecimalInf, valDecimalInf));

    value::releaseValue(tagDecimalInf, valDecimalInf);

    auto tagDoubleNan = value::TypeTags::NumberDouble;
    auto valDoubleNan = value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN());

    auto [tagDecimalNan, valDecimalNan] =
        value::makeCopyDecimal(mongo::Decimal128(std::numeric_limits<double>::quiet_NaN()));

    ASSERT_EQUALS(value::hashValue(tagDoubleNan, valDoubleNan),
                  value::hashValue(tagDecimalNan, valDecimalNan));

    value::releaseValue(tagDecimalNan, valDecimalNan);

    // Start with a relatively large number that still fits in 64 bits.
    int64_t num = 0x7000000000000000;
    // And is exactly representable as double (there is enough zeroes in the right part of the
    // number so we should be ok).
    auto asDbl = representAs<double>(num);
    ASSERT(asDbl);

    // Now "shift" the number to the left by 10 bits making it out of range for 64 bit integer.
    double asBigDbl = (*asDbl) * 1024.0;
    ASSERT(!representAs<int64_t>(asBigDbl));

    // Sanity check.
    ASSERT(asBigDbl / 1024.0 == (*asDbl));

    auto asDec = Decimal128(asBigDbl, Decimal128::kRoundTo34Digits);
    ASSERT(asDec.toDouble() == asBigDbl);

    auto tagDoubleBig = value::TypeTags::NumberDouble;
    auto valDoubleBig = value::bitcastFrom<double>(asBigDbl);

    auto [tagDecimalBig, valDecimalBig] = value::makeCopyDecimal(asDec);

    ASSERT_EQUALS(value::hashValue(tagDoubleBig, valDoubleBig),
                  value::hashValue(tagDecimalBig, valDecimalBig));

    value::releaseValue(tagDecimalBig, valDecimalBig);
}

TEST(SBEValues, HashCompound) {
    using namespace std::literals;
    {
        auto [tag1, val1] = value::makeNewArray();
        auto arr1 = value::getArrayView(val1);
        arr1->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-5));
        arr1->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-6));
        arr1->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-7));

        auto [tag2, val2] = value::makeNewArray();
        auto arr2 = value::getArrayView(val2);
        arr2->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-5.0));
        arr2->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-6.0));
        arr2->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-7.0));

        ASSERT_EQUALS(value::hashValue(tag1, val1), value::hashValue(tag2, val2));

        value::releaseValue(tag1, val1);
        value::releaseValue(tag2, val2);
    }
    {
        auto [tag1, val1] = value::makeNewObject();
        auto obj1 = value::getObjectView(val1);
        obj1->push_back("a"sv, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-5));
        obj1->push_back("b"sv, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-6));
        obj1->push_back("c"sv, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-7));

        auto [tag2, val2] = value::makeNewObject();
        auto obj2 = value::getObjectView(val2);
        obj2->push_back("a"sv, value::TypeTags::NumberDouble, value::bitcastFrom<double>(-5.0));
        obj2->push_back("b"sv, value::TypeTags::NumberDouble, value::bitcastFrom<double>(-6.0));
        obj2->push_back("c"sv, value::TypeTags::NumberDouble, value::bitcastFrom<double>(-7.0));

        ASSERT_EQUALS(value::hashValue(tag1, val1), value::hashValue(tag2, val2));

        value::releaseValue(tag1, val1);
        value::releaseValue(tag2, val2);
    }
}

TEST(SBEVM, Add) {
    {
        auto tagInt32 = value::TypeTags::NumberInt32;
        auto valInt32 = value::bitcastFrom<int32_t>(-7);

        auto tagInt64 = value::TypeTags::NumberInt64;
        auto valInt64 = value::bitcastFrom<int64_t>(-5);

        vm::CodeFragment code;
        code.appendConstVal(tagInt32, valInt32);
        code.appendConstVal(tagInt64, valInt64);
        code.appendAdd();

        vm::ByteCode interpreter;
        auto [owned, tag, val] = interpreter.run(&code);

        ASSERT_EQUALS(tag, value::TypeTags::NumberInt64);
        ASSERT_EQUALS(value::bitcastTo<int64_t>(val), -12);
    }
    {
        auto tagInt32 = value::TypeTags::NumberInt32;
        auto valInt32 = value::bitcastFrom<int32_t>(-7);

        auto tagDouble = value::TypeTags::NumberDouble;
        auto valDouble = value::bitcastFrom<double>(-5.0);

        vm::CodeFragment code;
        code.appendConstVal(tagInt32, valInt32);
        code.appendConstVal(tagDouble, valDouble);
        code.appendAdd();

        vm::ByteCode interpreter;
        auto [owned, tag, val] = interpreter.run(&code);

        ASSERT_EQUALS(tag, value::TypeTags::NumberDouble);
        ASSERT_EQUALS(value::bitcastTo<double>(val), -12.0);
    }
    {
        auto [tagDecimal, valDecimal] = value::makeCopyDecimal(mongo::Decimal128(-7.25));

        auto tagDouble = value::TypeTags::NumberDouble;
        auto valDouble = value::bitcastFrom<double>(-5.25);

        vm::CodeFragment code;
        code.appendConstVal(tagDecimal, valDecimal);
        code.appendConstVal(tagDouble, valDouble);
        code.appendAdd();

        vm::ByteCode interpreter;
        auto [owned, tag, val] = interpreter.run(&code);

        ASSERT_EQUALS(tag, value::TypeTags::NumberDecimal);
        ASSERT_EQUALS(value::bitcastTo<mongo::Decimal128>(val).toDouble(), -12.5);
        ASSERT_TRUE(owned);

        value::releaseValue(tag, val);
        value::releaseValue(tagDecimal, valDecimal);
    }
}

}  // namespace mongo::sbe
