/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <queue>

#include "mongo/db/exec/sbe/parser/parser.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {

namespace {
std::string valueDebugString(std::pair<value::TypeTags, value::Value> value) {
    std::stringstream stream;
    value::printValue(stream, value.first, value.second);
    return stream.str();
};
}  // namespace

#define APPEND_TWICE(BOB, NAME, VALUE)         \
    do {                                       \
        BOB.append(NAME "-ascending", VALUE);  \
        BOB.append(NAME "-descending", VALUE); \
    } while (false);

TEST(SBEKeyStringTest, Basic) {
    // Add interesting values to a BSON object. Note that we add each value twice: one will have an
    // "ascending" ordering, and the other will have a "descending" ordering.
    BSONObjBuilder bob;
    APPEND_TWICE(bob, "zeroInt", 0);
    APPEND_TWICE(bob, "oneByteInt", 0x10);
    APPEND_TWICE(bob, "twoByteInt", 0x1010);
    APPEND_TWICE(bob, "threeByteInt", 0x101010);
    APPEND_TWICE(bob, "fourByteInt", 0x10101010);
    APPEND_TWICE(bob, "fiveByteInt", 0x1010101010);
    APPEND_TWICE(bob, "sixByteInt", 0x101010101010);
    APPEND_TWICE(bob, "sevenByteInt", 0x10101010101010);
    APPEND_TWICE(bob, "eightByteInt", 0x1010101010101010);
    APPEND_TWICE(bob, "negativeZeroInt", 0);
    APPEND_TWICE(bob, "negativeOneByteInt", -0x10);
    APPEND_TWICE(bob, "negativeTwoByteInt", -0x1010);
    APPEND_TWICE(bob, "negativeThreeByteInt", -0x101010);
    APPEND_TWICE(bob, "negativeFourByteInt", -0x10101010);
    APPEND_TWICE(bob, "negativeFiveByteInt", -0x1010101010);
    APPEND_TWICE(bob, "negativeSixByteInt", -0x101010101010);
    APPEND_TWICE(bob, "negativeSevenByteInt", -0x10101010101010);
    APPEND_TWICE(bob, "negativeEightByteInt", -0x1010101010101010);
    APPEND_TWICE(bob, "boolFalse", false);
    APPEND_TWICE(bob, "boolTrue", true);
    APPEND_TWICE(bob, "doubleVal", 123.45);
    APPEND_TWICE(bob, "doubleInfinity", std::numeric_limits<double>::infinity());
    APPEND_TWICE(bob, "negativeDoubleInfinity", -std::numeric_limits<double>::infinity());
    APPEND_TWICE(bob, "decimalValue", Decimal128("01189998819991197253"));
    APPEND_TWICE(bob, "decimalInfinity", Decimal128::kPositiveInfinity);
    APPEND_TWICE(bob, "decimalNegativeInfinity", Decimal128::kNegativeInfinity);
    APPEND_TWICE(bob, "shortString", "str");
    APPEND_TWICE(bob, "longString", "I am the very model of a modern major general.");
    APPEND_TWICE(bob, "date", Date_t::fromMillisSinceEpoch(123));
    APPEND_TWICE(bob, "timestamp", Timestamp(123));

    bob.appendNull("null-ascending");
    bob.appendNull("null-descending");
    auto testValues = bob.done();

    // Copy each element from 'testValues' into a KeyString::Value. Each KeyString::Value has a
    // maximum number of components, so we have to break the elements up into groups.
    std::queue<std::tuple<KeyString::Value, Ordering, size_t>> keyStringQueue;
    std::vector<BSONElement> elements;
    testValues.elems(elements);

    for (size_t i = 0; i < elements.size(); i += Ordering::kMaxCompoundIndexKeys) {
        auto endBound = std::min(i + Ordering::kMaxCompoundIndexKeys, elements.size());

        BSONObjBuilder patternBob;
        for (auto j = i; j < endBound; ++j) {
            patternBob.append(elements[j].fieldNameStringData(), (j % 2 == 0) ? 1 : -1);
        }
        auto ordering = Ordering::make(patternBob.done());

        KeyString::Builder keyStringBuilder(KeyString::Version::V1, ordering);
        for (auto j = i; j < endBound; ++j) {
            keyStringBuilder.appendBSONElement(elements[j]);
        }
        keyStringQueue.emplace(keyStringBuilder.getValueCopy(), ordering, endBound - i);
    }

    // Set up an SBE expression that will compare one element in the 'testValues' BSON object with
    // one of the KeyString components.
    CompileCtx ctx;
    CoScanStage emptyStage;
    ctx.root = &emptyStage;

    // The expression takes three inputs:
    //   1) the BSON object,
    value::SlotId bsonObjSlot = 1;
    value::ViewOfValueAccessor bsonObjAccessor;
    ctx.pushCorrelated(bsonObjSlot, &bsonObjAccessor);

    //  2) the field name corresponding to the BSON element,
    value::SlotId fieldNameSlot = 2;
    value::OwnedValueAccessor fieldNameAccessor;
    ctx.pushCorrelated(fieldNameSlot, &fieldNameAccessor);

    //  3) and the KeyString component.
    value::SlotId keyStringComponentSlot = 3;
    value::ViewOfValueAccessor keyStringComponentAccessor;
    ctx.pushCorrelated(keyStringComponentSlot, &keyStringComponentAccessor);

    auto comparisonExpr = makeE<EPrimBinary>(
        EPrimBinary::eq,
        makeE<EVariable>(keyStringComponentSlot),
        makeE<EFunction>("getField",
                         makeEs(makeE<EVariable>(bsonObjSlot), makeE<EVariable>(fieldNameSlot))));
    auto compiledComparison = comparisonExpr->compile(ctx);


    bsonObjAccessor.reset(value::TypeTags::bsonObject, value::bitcastFrom(testValues.objdata()));
    std::vector<sbe::value::ViewOfValueAccessor> keyStringValues;
    BufBuilder builder;
    for (auto&& element : testValues) {
        while (keyStringValues.empty()) {
            ASSERT(!keyStringQueue.empty());

            auto [keyString, ordering, size] = keyStringQueue.front();
            keyStringQueue.pop();

            builder.reset();
            keyStringValues.resize(size);
            readKeyStringValueIntoAccessors(keyString, ordering, &builder, &keyStringValues);
        }

        auto [componentTag, componentVal] = keyStringValues.front().getViewOfValue();
        keyStringComponentAccessor.reset(componentTag, componentVal);
        keyStringValues.erase(keyStringValues.begin());

        auto [fieldNameTag, fieldNameVal] = value::makeNewString(element.fieldName());
        fieldNameAccessor.reset(fieldNameTag, fieldNameVal);

        vm::ByteCode vm;
        auto result = vm.runPredicate(compiledComparison.get());
        ASSERT(result) << "BSONElement (" << element << ") failed to match KeyString component ("
                       << valueDebugString(std::make_pair(componentTag, componentVal)) << ")";
    }

    ASSERT(keyStringValues.empty());
    ASSERT(keyStringQueue.empty());
}

TEST(SBEKeyStringTest, KeyComponentInclusion) {
    KeyString::Builder keyStringBuilder(KeyString::Version::V1, KeyString::ALL_ASCENDING);
    keyStringBuilder.appendNumberLong(12345);  // Included
    keyStringBuilder.appendString("I've information vegetable, animal, and mineral"_sd);
    keyStringBuilder.appendString(
        "I know the kings of England, and I quote the fights historical"_sd);  // Included
    keyStringBuilder.appendString("From Marathon to Waterloo, in order categorical");
    auto keyString = keyStringBuilder.getValueCopy();

    IndexKeysInclusionSet indexKeysToInclude;
    indexKeysToInclude.set(0);
    indexKeysToInclude.set(2);

    std::vector<value::ViewOfValueAccessor> accessors;
    accessors.resize(2);

    BufBuilder builder;
    readKeyStringValueIntoAccessors(
        keyString, KeyString::ALL_ASCENDING, &builder, &accessors, indexKeysToInclude);

    ASSERT(std::make_pair(value::TypeTags::NumberInt64, value::bitcastFrom(12345)) ==
           accessors[0].getViewOfValue())
        << "Incorrect value from accessor: " << valueDebugString(accessors[0].getViewOfValue());

    auto value = accessors[1].getViewOfValue();
    ASSERT(value::isString(value.first) &&
           ("I know the kings of England, and I quote the fights historical" ==
            value::getStringView(value.first, value.second)))
        << "Incorrect value from accessor: " << valueDebugString(value);
}

}  // namespace mongo::sbe
