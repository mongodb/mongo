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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ostream>
#include <queue>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace mongo::sbe {

namespace {
using SBEKeyStringTest = EExpressionTestFixture;

std::string valueDebugString(std::pair<value::TypeTags, value::Value> value) {
    std::stringstream stream;
    stream << std::make_pair(value.first, value.second);
    return stream.str();
};
}  // namespace

#define APPEND_TWICE(BOB, NAME, VALUE)         \
    do {                                       \
        BOB.append(NAME "-ascending", VALUE);  \
        BOB.append(NAME "-descending", VALUE); \
    } while (false);

TEST_F(SBEKeyStringTest, Basic) {
    // Add interesting values to a BSON object. Note that we add each value twice: one will have an
    // "ascending" ordering, and the other will have a "descending" ordering.
    BSONObjBuilder bob;
    APPEND_TWICE(bob, "zeroInt", 0);
    APPEND_TWICE(bob, "oneByteInt", 0x10);
    APPEND_TWICE(bob, "twoByteInt", 0x1010);
    APPEND_TWICE(bob, "threeByteInt", 0x101010);
    APPEND_TWICE(bob, "fourByteInt", 0x10101010);
    APPEND_TWICE(bob, "fiveByteInt", int64_t{0x1010101010});
    APPEND_TWICE(bob, "sixByteInt", int64_t{0x101010101010});
    APPEND_TWICE(bob, "sevenByteInt", int64_t{0x10101010101010});
    APPEND_TWICE(bob, "eightByteInt", int64_t{0x1010101010101010});
    APPEND_TWICE(bob, "negativeZeroInt", 0);
    APPEND_TWICE(bob, "negativeOneByteInt", -0x10);
    APPEND_TWICE(bob, "negativeTwoByteInt", -0x1010);
    APPEND_TWICE(bob, "negativeThreeByteInt", -0x101010);
    APPEND_TWICE(bob, "negativeFourByteInt", -0x10101010);
    APPEND_TWICE(bob, "negativeFiveByteInt", int64_t{-0x1010101010});
    APPEND_TWICE(bob, "negativeSixByteInt", int64_t{-0x101010101010});
    APPEND_TWICE(bob, "negativeSevenByteInt", int64_t{-0x10101010101010});
    APPEND_TWICE(bob, "negativeEightByteInt", int64_t{-0x1010101010101010});
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
    APPEND_TWICE(bob, "symbol", BSONSymbol("I am the very model of a modern major general."));
    APPEND_TWICE(bob, "date", Date_t::fromMillisSinceEpoch(123));
    APPEND_TWICE(bob, "timestamp", Timestamp(123));
    APPEND_TWICE(bob, "binData", BSONBinData("\xde\xad\xbe\xef", 4, BinDataGeneral));
    APPEND_TWICE(bob, "code", BSONCode("function test() { return 'Hello world!'; }"));
    APPEND_TWICE(bob, "dbref", BSONDBRef("db.c", OID("010203040506070809101112")));
    APPEND_TWICE(
        bob, "cws", BSONCodeWScope("function test() { return 'Hello world!'; }", BSONObj()));

    bob.appendNull("null-ascending");
    bob.appendNull("null-descending");
    auto testValues = bob.done();

    // Copy each element from 'testValues' into a key_string::Value. Each key_string::Value has a
    // maximum number of components, so we have to break the elements up into groups.
    std::queue<std::tuple<key_string::Value, Ordering, size_t>> keyStringQueue;
    std::vector<BSONElement> elements;
    testValues.elems(elements);

    for (size_t i = 0; i < elements.size(); i += Ordering::kMaxCompoundIndexKeys) {
        auto endBound = std::min(i + Ordering::kMaxCompoundIndexKeys, elements.size());

        BSONObjBuilder patternBob;
        for (auto j = i; j < endBound; ++j) {
            patternBob.append(elements[j].fieldNameStringData(), (j % 2 == 0) ? 1 : -1);
        }
        auto ordering = Ordering::make(patternBob.done());

        key_string::Builder keyStringBuilder(key_string::Version::V1, ordering);
        for (auto j = i; j < endBound; ++j) {
            keyStringBuilder.appendBSONElement(elements[j]);
        }
        keyStringBuilder.appendRecordId(RecordId{0});
        keyStringQueue.emplace(keyStringBuilder.getValueCopy(), ordering, endBound - i);
    }

    // Set up an SBE expression that will compare one element in the 'testValues' BSON object with
    // one of the KeyString components.
    CompileCtx ctx{std::make_unique<RuntimeEnvironment>()};
    CoScanStage emptyStage{kEmptyPlanNodeId};
    ctx.root = &emptyStage;

    // The expression takes three inputs:
    //   1) the BSON object,
    value::ViewOfValueAccessor bsonObjAccessor;
    auto bsonObjSlot = bindAccessor(&bsonObjAccessor);

    //  2) the field name corresponding to the BSON element,
    value::OwnedValueAccessor fieldNameAccessor;
    auto fieldNameSlot = bindAccessor(&fieldNameAccessor);

    //  3) and the KeyString component.
    value::ViewOfValueAccessor keyStringComponentAccessor;
    auto keyStringComponentSlot = bindAccessor(&keyStringComponentAccessor);

    auto comparisonExpr = makeE<EPrimBinary>(
        EPrimBinary::eq,
        makeE<EVariable>(keyStringComponentSlot),
        makeE<EFunction>("getField",
                         makeEs(makeE<EVariable>(bsonObjSlot), makeE<EVariable>(fieldNameSlot))));
    auto compiledExpr = compileExpression(*comparisonExpr);

    bsonObjAccessor.reset(value::TypeTags::bsonObject,
                          value::bitcastFrom<const char*>(testValues.objdata()));
    std::vector<sbe::value::OwnedValueAccessor> keyStringValues;
    BufBuilder builder;
    for (auto&& element : testValues) {
        while (keyStringValues.empty()) {
            ASSERT(!keyStringQueue.empty());

            auto [keyString, ordering, size] = keyStringQueue.front();
            keyStringQueue.pop();

            builder.reset();
            keyStringValues.resize(size);
            SortedDataKeyValueView view{keyString.getView(),
                                        keyString.getRecordIdView(),
                                        keyString.getTypeBitsView(),
                                        keyString.getVersion(),
                                        true};
            readKeyStringValueIntoAccessors(view, ordering, &builder, &keyStringValues);
        }

        auto [componentTag, componentVal] = keyStringValues.front().getViewOfValue();
        keyStringComponentAccessor.reset(componentTag, componentVal);
        keyStringValues.erase(keyStringValues.begin());

        auto [fieldNameTag, fieldNameVal] = value::makeNewString(element.fieldName());
        fieldNameAccessor.reset(fieldNameTag, fieldNameVal);

        auto result = runCompiledExpressionPredicate(compiledExpr.get());
        ASSERT(result) << "BSONElement (" << element << ") failed to match KeyString component ("
                       << valueDebugString(std::make_pair(componentTag, componentVal)) << ")";
    }

    ASSERT(keyStringValues.empty());
    ASSERT(keyStringQueue.empty());
}

TEST(SimpleSBEKeyStringTest, KeyComponentInclusion) {
    key_string::Builder keyStringBuilder(key_string::Version::V1, key_string::ALL_ASCENDING);
    keyStringBuilder.appendNumberLong(12345);  // Included
    keyStringBuilder.appendString("I've information vegetable, animal, and mineral"_sd);
    keyStringBuilder.appendString(
        "I know the kings of England, and I quote the fights historical"_sd);  // Included
    keyStringBuilder.appendString("From Marathon to Waterloo, in order categorical");
    keyStringBuilder.appendRecordId(RecordId{0});

    IndexKeysInclusionSet indexKeysToInclude;
    indexKeysToInclude.set(0);
    indexKeysToInclude.set(2);

    std::vector<value::OwnedValueAccessor> accessors;
    accessors.resize(2);

    auto keySize = key_string::getKeySize(
        keyStringBuilder.getView(), key_string::ALL_ASCENDING, keyStringBuilder.version);
    SortedDataKeyValueView view{keyStringBuilder.getView(),
                                keyStringBuilder.getView().subspan(keySize),
                                keyStringBuilder.getTypeBits().getView(),
                                keyStringBuilder.version,
                                true};
    BufBuilder builder;
    readKeyStringValueIntoAccessors(
        view, key_string::ALL_ASCENDING, &builder, &accessors, indexKeysToInclude);

    auto value = accessors[0].getViewOfValue();
    ASSERT(value::TypeTags::NumberInt64 == value.first &&
           12345 == value::bitcastTo<int64_t>(value.second))
        << "Incorrect value from accessor: " << valueDebugString(value);

    value = accessors[1].getViewOfValue();
    ASSERT(value::isString(value.first) &&
           ("I know the kings of England, and I quote the fights historical" ==
            value::getStringView(value.first, value.second)))
        << "Incorrect value from accessor: " << valueDebugString(value);
}

}  // namespace mongo::sbe
