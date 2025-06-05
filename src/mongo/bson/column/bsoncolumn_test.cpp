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

#include "mongo/bson/column/bsoncolumn.h"

#include "mongo/bson/bsonelement.h"

#include <absl/numeric/int128.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/column/bsoncolumn_expressions.h"
#include "mongo/bson/column/bsoncolumn_fuzzer_util.h"
#include "mongo/bson/column/bsoncolumn_test_util.h"
#include "mongo/bson/column/bsoncolumnbuilder.h"
#include "mongo/bson/column/simple8b_builder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bsonobj_traversal.h"
#include "mongo/bson/util/builder.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/time_support.h"
#include "mongo/util/tracking/context.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <forward_list>
#include <limits>
#include <string>

namespace mongo::bsoncolumn {
namespace {

void assertBinaryEqual(BSONBinData finalizedColumn, const BufBuilder& buffer) {
    ASSERT_EQ(finalizedColumn.type, BinDataType::Column);
    ASSERT_EQ(finalizedColumn.length, buffer.len());
    ASSERT_EQ(memcmp(finalizedColumn.data, buffer.buf(), finalizedColumn.length), 0);
}

class BSONColumnTest : public unittest::Test {
public:
    ~BSONColumnTest() override {
        auto& trackingContext = trackingContextChecker.trackingContext;
        auto allocated = trackingContext.allocated();
        ASSERT_GT(allocated, 0);

        // Move construct and move assign builders. These operations may allocate memory on certain
        // platforms/implementations so we cannot check the exact memory usage in an platform
        // independent way. But we make sure that the memory usage is 0 when all these are torn down
        // to ensure there's no memory tracking leaks after moving.
        BSONColumnBuilder<tracking::Allocator<void>> moveContructBuilder{std::move(cb)};
        BSONColumnBuilder<tracking::Allocator<void>> moveAssignBuilder{
            trackingContextChecker.trackingContext.makeAllocator<void>()};
        moveAssignBuilder = std::move(moveContructBuilder);
    }

    BSONElement createBSONColumn(const char* buffer, int size) {
        BSONObjBuilder ob;
        ob.appendBinData(""_sd, size, BinDataType::Column, buffer);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    template <typename T>
    BSONElement _createElement(T val) {
        BSONObjBuilder ob;
        ob.append("0"_sd, val);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createElementDouble(double val) {
        BSONObjBuilder ob;
        ob.append("0"_sd, val);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createObjectId(OID val) {
        return _createElement(val);
    }

    BSONElement createTimestamp(Timestamp val) {
        return _createElement(val);
    }

    BSONElement createElementInt64(int64_t val) {
        return _createElement(val);
    }

    BSONElement createElementInt32(int32_t val) {
        return _createElement(val);
    }

    BSONElement createElementDecimal128(Decimal128 val) {
        return _createElement(val);
    }

    BSONElement createDate(Date_t dt) {
        return _createElement(dt);
    }

    BSONElement createBool(bool b) {
        return _createElement(b);
    }

    BSONElement createElementMinKey() {
        BSONObjBuilder ob;
        ob.appendMinKey("0"_sd);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createElementMaxKey() {
        BSONObjBuilder ob;
        ob.appendMaxKey("0"_sd);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createNull() {
        BSONObjBuilder ob;
        ob.appendNull("0"_sd);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createUndefined() {
        BSONObjBuilder ob;
        ob.appendUndefined("0"_sd);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createRegex(StringData pattern = "", StringData options = "") {
        BSONObjBuilder ob;
        ob.appendRegex("0"_sd, pattern, options);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createDBRef(StringData ns, const OID& oid) {
        BSONObjBuilder ob;
        ob.appendDBRef("0"_sd, ns, oid);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createElementCode(StringData code) {
        BSONObjBuilder ob;
        ob.appendCode("0"_sd, code);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createCodeWScope(StringData code, const BSONObj& scope) {
        BSONObjBuilder ob;
        ob.appendCodeWScope("0"_sd, code, scope);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createSymbol(StringData symbol) {
        BSONObjBuilder ob;
        ob.appendSymbol("0"_sd, symbol);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createElementBinData(BinDataType binDataType, const std::vector<uint8_t>& val) {
        BSONObjBuilder ob;
        ob.appendBinData("f", val.size(), binDataType, val.data());
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createElementString(StringData val) {
        return _createElement(val);
    }

    BSONElement createElementObj(BSONObj obj) {
        return _createElement(obj);
    }

    BSONElement createElementArray(BSONArray arr) {
        return _createElement(arr);
    }

    BSONElement createElementArrayAsObject(BSONArray arr) {
        return _createElement(*static_cast<BSONObj*>(&arr));
    }

    static boost::optional<uint128_t> deltaBinData(BSONElement val, BSONElement prev) {
        if (val.binaryEqualValues(prev)) {
            return uint128_t(0);
        }

        int valSize;
        int prevSize;
        const char* valBinary = val.binData(valSize);
        const char* prevBinary = prev.binData(prevSize);
        if (valSize != prevSize || valSize > 16) {
            return boost::none;
        }

        return Simple8bTypeUtil::encodeInt128(
            *Simple8bTypeUtil::encodeBinary(valBinary, valSize) -
            *Simple8bTypeUtil::encodeBinary(prevBinary, prevSize));
    }

    static uint128_t deltaString(BSONElement val, BSONElement prev) {
        return Simple8bTypeUtil::encodeInt128(
            Simple8bTypeUtil::encodeString(val.valueStringData()).value_or(0) -
            Simple8bTypeUtil::encodeString(prev.valueStringData()).value_or(0));
    }

    static uint64_t deltaInt32(BSONElement val, BSONElement prev) {
        return Simple8bTypeUtil::encodeInt64(val.Int() - prev.Int());
    }

    static uint64_t deltaInt64(BSONElement val, BSONElement prev) {
        return Simple8bTypeUtil::encodeInt64(
            static_cast<long long>(static_cast<unsigned long long>(val.Long()) -
                                   static_cast<unsigned long long>(prev.Long())));
    }

    static uint64_t deltaDouble(BSONElement val, BSONElement prev, double scaleFactor) {
        size_t scaleIndex = 0;
        for (; scaleIndex < Simple8bTypeUtil::kScaleMultiplier.size(); ++scaleIndex) {
            if (Simple8bTypeUtil::kScaleMultiplier[scaleIndex] == scaleFactor)
                break;
        }
        return Simple8bTypeUtil::encodeInt64(
            *Simple8bTypeUtil::encodeDouble(val.Double(), scaleIndex) -
            *Simple8bTypeUtil::encodeDouble(prev.Double(), scaleIndex));
    }

    static uint64_t deltaDoubleMemory(BSONElement val, BSONElement prev) {
        return Simple8bTypeUtil::encodeInt64(
            static_cast<int64_t>(static_cast<uint64_t>(*Simple8bTypeUtil::encodeDouble(
                                     val.Double(), Simple8bTypeUtil::kMemoryAsInteger)) -
                                 static_cast<uint64_t>(*Simple8bTypeUtil::encodeDouble(
                                     prev.Double(), Simple8bTypeUtil::kMemoryAsInteger))));
    }

    static bool simple8bPossible(uint64_t val) {
        Simple8bBuilder<uint64_t> b;
        return b.append(val, [](uint64_t block) {});
    }

    static uint64_t deltaOfDelta(int64_t delta, int64_t prevDelta) {
        return Simple8bTypeUtil::encodeInt64(delta - prevDelta);
    }

    static uint64_t deltaOfDeltaObjectId(BSONElement val, BSONElement prev, BSONElement prevprev) {
        ASSERT_EQ(memcmp(val.OID().getInstanceUnique().bytes,
                         prev.OID().getInstanceUnique().bytes,
                         OID::kInstanceUniqueSize),
                  0);

        ASSERT_EQ(memcmp(prevprev.OID().getInstanceUnique().bytes,
                         prev.OID().getInstanceUnique().bytes,
                         OID::kInstanceUniqueSize),
                  0);

        int64_t delta = Simple8bTypeUtil::encodeObjectId(val.OID()) -
            Simple8bTypeUtil::encodeObjectId(prev.OID());
        int64_t prevDelta = Simple8bTypeUtil::encodeObjectId(prev.OID()) -
            Simple8bTypeUtil::encodeObjectId(prevprev.OID());
        return deltaOfDelta(delta, prevDelta);
    }

    static uint128_t deltaDecimal128(BSONElement val, BSONElement prev) {
        return Simple8bTypeUtil::encodeInt128(Simple8bTypeUtil::encodeDecimal128(val.Decimal()) -
                                              Simple8bTypeUtil::encodeDecimal128(prev.Decimal()));
    }


    uint64_t deltaOfDeltaTimestamp(BSONElement val, BSONElement prev) {
        return Simple8bTypeUtil::encodeInt64(val.timestamp().asULL() - prev.timestamp().asULL());
    }

    static uint64_t deltaOfDeltaTimestamp(BSONElement val, BSONElement prev, BSONElement prevprev) {
        int64_t prevTimestampDelta = prev.timestamp().asULL() - prevprev.timestamp().asULL();
        int64_t currTimestampDelta = val.timestamp().asULL() - prev.timestamp().asULL();
        return deltaOfDelta(currTimestampDelta, prevTimestampDelta);
    }

    template <typename It>
    static std::vector<boost::optional<uint64_t>> deltaInt64(It begin, It end, BSONElement prev) {
        std::vector<boost::optional<uint64_t>> deltas;
        for (; begin != end; ++begin) {
            if (!begin->eoo()) {
                deltas.push_back(deltaInt64(*begin, prev));
                prev = *begin;
            } else {
                deltas.push_back(boost::none);
            }
        }
        return deltas;
    }

    template <typename It>
    static std::vector<boost::optional<uint64_t>> deltaDouble(It begin,
                                                              It end,
                                                              BSONElement prev,
                                                              double scaleFactor) {
        std::vector<boost::optional<uint64_t>> deltas;
        for (; begin != end; ++begin) {
            if (!begin->eoo()) {
                deltas.push_back(deltaDouble(*begin, prev, scaleFactor));
                prev = *begin;
            } else {
                deltas.push_back(boost::none);
            }
        }
        return deltas;
    }

    template <typename It>
    static std::vector<boost::optional<uint64_t>> deltaDoubleMemory(It begin,
                                                                    It end,
                                                                    BSONElement prev) {
        std::vector<boost::optional<uint64_t>> deltas;
        for (; begin != end; ++begin) {
            if (!begin->eoo()) {
                deltas.push_back(deltaDoubleMemory(*begin, prev));
                prev = *begin;
            } else {
                deltas.push_back(boost::none);
            }
        }
        return deltas;
    }

    template <typename It>
    static std::vector<boost::optional<uint128_t>> deltaString(It begin, It end, BSONElement prev) {
        std::vector<boost::optional<uint128_t>> deltas;
        for (; begin != end; ++begin) {
            if (!begin->eoo()) {
                deltas.push_back(deltaString(*begin, prev));
                prev = *begin;
            } else {
                deltas.push_back(boost::none);
            }
        }
        return deltas;
    }

    static uint64_t deltaBool(BSONElement val, BSONElement prev) {
        return Simple8bTypeUtil::encodeInt64(val.Bool() - prev.Bool());
    }

    static uint64_t deltaOfDeltaDate(BSONElement val, BSONElement prev, BSONElement prevprev) {
        int64_t delta = val.Date().toMillisSinceEpoch() - prev.Date().toMillisSinceEpoch();
        int64_t prevDelta = prev.Date().toMillisSinceEpoch() - prevprev.Date().toMillisSinceEpoch();
        return deltaOfDelta(delta, prevDelta);
    }

    template <typename It>
    static std::vector<boost::optional<uint64_t>> deltaOfDeltaDates(It begin,
                                                                    It end,
                                                                    BSONElement prev,
                                                                    BSONElement prevprev) {
        std::vector<boost::optional<uint64_t>> deltas;
        for (; begin != end; ++begin) {
            deltas.push_back(deltaOfDeltaDate(*begin, prev, prevprev));
            prevprev = prev;
            prev = *begin;
        }
        return deltas;
    }

    static void appendLiteral(BufBuilder& builder, BSONElement elem) {
        // BSON Type byte
        builder.appendChar(stdx::to_underlying(elem.type()));

        // Null terminator for field name
        builder.appendChar('\0');

        // Element value
        builder.appendBuf(elem.value(), elem.valuesize());
    }

    static void appendSimple8bControl(BufBuilder& builder, uint8_t control, uint8_t count) {
        builder.appendChar(control << 4 | count);
    }

    static void appendInterleavedStartLegacy(BufBuilder& builder, BSONObj reference) {
        builder.appendChar((char)0xF0);
        builder.appendBuf(reference.objdata(), reference.objsize());
    }

    static void appendInterleavedStart(BufBuilder& builder, BSONObj reference) {
        builder.appendChar((char)0xF1);
        builder.appendBuf(reference.objdata(), reference.objsize());
    }

    static void appendInterleavedStartArrayRoot(BufBuilder& builder, BSONObj reference) {
        builder.appendChar((char)0xF2);
        builder.appendBuf(reference.objdata(), reference.objsize());
    }

    template <typename T>
    static void _appendSimple8bBlock(BufBuilder& builder, boost::optional<T> val) {
        auto prev = builder.len();
        auto writeFn = [&builder](uint64_t block) {
            builder.appendNum(block);
            return true;
        };
        Simple8bBuilder<T> s8bBuilder;
        if (val) {
            s8bBuilder.append(*val, writeFn);
        } else {
            s8bBuilder.skip(writeFn);
        }

        s8bBuilder.flush(writeFn);
        ASSERT_EQ(builder.len() - prev, sizeof(uint64_t));
    }

    static void appendSimple8bBlock64(BufBuilder& builder, boost::optional<uint64_t> val) {
        _appendSimple8bBlock<uint64_t>(builder, val);
    }

    static void appendSimple8bBlock128(BufBuilder& builder, boost::optional<uint128_t> val) {
        _appendSimple8bBlock<uint128_t>(builder, val);
    }

    template <typename T>
    static void _appendSimple8bBlocks(BufBuilder& builder,
                                      const std::vector<boost::optional<T>>& vals,
                                      uint32_t expectedNum) {
        auto prev = builder.len();
        auto writeFn = [&builder](uint64_t block) {
            builder.appendNum(block);
            return true;
        };
        Simple8bBuilder<T> s8bBuilder;
        for (auto val : vals) {
            if (val) {
                s8bBuilder.append(*val, writeFn);
            } else {
                s8bBuilder.skip(writeFn);
            }
        }
        s8bBuilder.flush(writeFn);
        ASSERT_EQ((builder.len() - prev) / sizeof(uint64_t), expectedNum);
    }

    static void appendSimple8bBlocks64(BufBuilder& builder,
                                       const std::vector<boost::optional<uint64_t>>& vals,
                                       uint32_t expectedNum) {
        _appendSimple8bBlocks<uint64_t>(builder, vals, expectedNum);
    }

    static void appendSimple8bBlocks128(BufBuilder& builder,
                                        const std::vector<boost::optional<uint128_t>> vals,
                                        uint32_t expectedNum) {
        _appendSimple8bBlocks<uint128_t>(builder, vals, expectedNum);
    }

    static void appendSimple8bRLE(BufBuilder& builder, int elemCount) {
        ASSERT(elemCount % 120 == 0);
        ASSERT(elemCount / 120 <= 16);

        uint64_t block = (elemCount / 120) - 1;
        builder.appendNum(block << 4 | simple8b_internal::kRleSelector);
    }

    static void appendEOO(BufBuilder& builder) {
        builder.appendChar(stdx::to_underlying(BSONType::eoo));
    }

    static void convertAndAssertSBEEquals(sbe::bsoncolumn::SBEColumnMaterializer::Element& actual,
                                          const BSONElement& expected) {
        auto expectedSBE = sbe::bson::convertFrom<true>(expected);
        if (actual.first == sbe::value::TypeTags::StringSmall) {
            // Generic conversion won't produce StringSmall from BSONElements, but
            // SBEColumnMaterializer will, don't compare the type tag for that case.
            ASSERT_EQ(expectedSBE.first, sbe::value::TypeTags::bsonString);
        } else {
            ASSERT_EQ(actual.first, expectedSBE.first);
        }
        ASSERT(areSBEBinariesEqual(actual, expectedSBE));
    }

    static void verifyColumnReopenFromBinary(const char* buffer, size_t size) {
        BSONColumn column(buffer, size);

        BSONColumnBuilder reference;
        for (auto&& elem : column) {
            reference.append(elem);
        }

        BSONColumnBuilder<> reopen(buffer, size);
        [[maybe_unused]] auto diff = reference.intermediate();

        // Verify that the internal state is identical to the reference builder
        invariant(reopen.isInternalStateIdentical(reference));
    }

    static void verifyBinary(BSONBinData columnBinary,
                             const BufBuilder& expected,
                             bool testReopen = true) {
        ASSERT_EQ(columnBinary.type, BinDataType::Column);

        auto buf = expected.buf();
        ASSERT_EQ(columnBinary.length, expected.len());
        for (int i = 0; i < columnBinary.length; ++i) {
            ASSERT_EQ(*(reinterpret_cast<const char*>(columnBinary.data) + i), buf[i]);
        }
        ASSERT_EQ(memcmp(columnBinary.data, buf, columnBinary.length), 0);

        // Verify BSONColumnBuilder::last
        {
            BSONColumnBuilder cb;
            // Initial state returns eoo
            ASSERT_TRUE(cb.last().eoo());

            BSONColumn column(columnBinary);
            BSONElement last;
            for (auto&& elem : column) {
                cb.append(elem);
                // Last does not consider skip
                if (!elem.eoo()) {
                    last = elem;
                }

                if (last.eoo()) {
                    // Only skips have been encountered, last() should continue to return EOO
                    ASSERT_TRUE(cb.last().eoo());
                } else if (last.type() != BSONType::object && last.type() != BSONType::array) {
                    // Empty objects and arrays _may_ be encoded as scalar depending on what else
                    // has been added to the builder. This makes this case difficult to test and we
                    // just test the scalar types instead.
                    ASSERT_FALSE(cb.last().eoo());
                    ASSERT_TRUE(last.binaryEqualValues(cb.last()));
                }
            }
        }

        // Verify BSONColumnBuilder::intermediate
        {
            // Test intermediate when called between every append to ensure we get the same binary
            // compared to when no intermediate was used.
            {
                BufBuilder buffer;
                BSONColumnBuilder cb;
                BSONColumnBuilder reference;
                std::vector<BSONElement> elems;

                BSONColumn c(columnBinary);
                bool empty = true;
                for (auto&& elem : c) {
                    elems.push_back(elem);
                    cb.append(elem);
                    reference.append(elem);

                    auto diff = cb.intermediate();

                    ASSERT_GTE(buffer.len(), diff.offset());
                    buffer.setlen(diff.offset());
                    buffer.appendBuf(diff.data(), diff.size());
                    if (testReopen) {
                        verifyDecompressionBasic(buffer, elems);
                    }

                    empty = false;
                }

                // If there was nothing in the column, we need to make sure at least one
                // intermediate was called
                if (empty) {
                    auto diff = cb.intermediate();
                    ASSERT_EQ(diff.offset(), 0);
                    buffer.appendBuf(diff.data(), diff.size());
                }

                // Compare the binary with one obtained using finalize
                assertBinaryEqual(reference.finalize(), buffer);
            }

            // Test intermediate when called between every other append to ensure we get the same
            // binary compared to when no intermediate was used.
            {
                BufBuilder buffer;
                BSONColumnBuilder cb;
                BSONColumnBuilder reference;
                std::vector<BSONElement> elems;

                BSONColumn c(columnBinary);

                int num = 0;
                for (auto it = c.begin(); it != c.end(); ++it, ++num) {
                    elems.push_back(*it);
                    cb.append(*it);
                    reference.append(*it);

                    if (num % 2 == 1)
                        continue;

                    auto diff = cb.intermediate();

                    ASSERT_GTE(buffer.len(), diff.offset());
                    buffer.setlen(diff.offset());
                    buffer.appendBuf(diff.data(), diff.size());
                    if (testReopen) {
                        verifyDecompressionBasic(buffer, elems);
                    }
                }

                // One last intermediate to ensure all data is put into binary
                auto diff = cb.intermediate();
                ASSERT_GTE(buffer.len(), diff.offset());
                buffer.setlen(diff.offset());
                buffer.appendBuf(diff.data(), diff.size());

                // Compare the binary with one obtained using finalize
                assertBinaryEqual(reference.finalize(), buffer);
            }

            // Divide the range into two blocks where intermediate is called in between. We do all
            // combinations of dividing the ranges so this test has quadratic complexity. Limit it
            // to binaries with a limited number of elements.
            size_t num = BSONColumn(columnBinary).size();
            if (num > 0 && num < 1000) {
                // Iterate over all elements and validate intermediate in all combinations
                for (size_t i = 1; i <= num; ++i) {
                    BufBuilder buffer;
                    BSONColumnBuilder cb;
                    BSONColumnBuilder reference;

                    // Append initial data to our builder
                    BSONColumn c(columnBinary);
                    auto it = c.begin();
                    for (size_t j = 0; j < i; ++j, ++it) {
                        cb.append(*it);
                        reference.append(*it);
                    }

                    // Call intermediate to obtain the initial binary
                    auto diff = cb.intermediate();
                    ASSERT_EQ(diff.offset(), 0);
                    buffer.appendBuf(diff.data(), diff.size());

                    // Append the rest of the data
                    BufBuilder buffer2;
                    for (size_t j = i; j < num; ++j, ++it) {
                        cb.append(*it);
                        reference.append(*it);
                    }

                    // Call intermediate to obtain rest of the binary
                    diff = cb.intermediate();
                    // Start writing at the provided offset
                    ASSERT_GTE(buffer.len(), diff.offset());
                    buffer.setlen(diff.offset());
                    buffer.appendBuf(diff.data(), diff.size());

                    // We should now have added all our data.
                    ASSERT(it == c.end());

                    // Compare the binary with one obtained using finalize
                    assertBinaryEqual(reference.finalize(), buffer);
                }
            }
        }

        if (testReopen) {
            BSONColumn c(columnBinary);
            size_t num = c.size();

            // Validate the BSONColumnBuilder constructor that initializes from a compressed binary.
            // Its state should be identical to a builder that never called finalize() for these
            // elements.
            for (size_t i = 0; i <= num; ++i) {
                BSONColumnBuilder before;
                auto it = c.begin();
                for (size_t j = 0; j < i; ++j, ++it) {
                    before.append(*it);
                }

                auto intermediate = before.finalize();
                verifyColumnReopenFromBinary(reinterpret_cast<const char*>(intermediate.data),
                                             intermediate.length);
            }
        }
    }

    static void verifyDecompressionInterleaved(const std::vector<BSONElement>& input,
                                               bool testPathDecompression) {
        BSONColumnBuilder interleavedCb;

        // Store the BSONObj to ensure the BSONElements stay in scope.
        std::vector<BSONObj> expected;
        expected.reserve(input.size());

        // Wrap each input in a BSONObj.
        for (auto&& elem : input) {
            BSONObjBuilder ob;
            ob.append("0"_sd, (elem.eoo() ? BSONObj{} : BSON("fp" << elem)));
            expected.push_back(ob.obj());
            interleavedCb.append(expected.back().firstElement());
        }
        auto interleavedBinary = interleavedCb.finalize();

        // Set up the iterative and block API.
        BSONColumn col(interleavedBinary);
        boost::intrusive_ptr allocator{new BSONElementStorage()};
        std::vector<BSONElement> collection;
        BSONColumnBlockBased block((const char*)interleavedBinary.data, interleavedBinary.length);
        block.decompress<BSONElementMaterializer, std::vector<BSONElement>>(collection, allocator);

        // Verify the decompressed elements are the same and expected from both APIs.
        ASSERT(col.size() == input.size() && collection.size() == input.size());
        auto iterRes = col.begin();
        auto blockRes = collection.begin();
        std::vector<BSONElement> pathAPIElements;
        pathAPIElements.reserve(input.size());
        for (auto&& expectedObj : expected) {
            pathAPIElements.push_back(expectedObj.firstElement());
            ASSERT(expectedObj.firstElement().binaryEqualValues(*iterRes) &&
                   (*iterRes).binaryEqualValues(*blockRes));
            ++iterRes;
            ++blockRes;
        }

        // If supported, validate the path API with the data created can handle decompressing
        // duplicate paths.
        if (testPathDecompression) {
            std::vector<TestPath> testPaths{TestPath{{"fp"}}, TestPath{{"fp"}}};
            verifyDecompressPathFast(interleavedBinary, pathAPIElements, testPaths);
        }
    }

    static void verifyDecompressionBasic(const BufBuilder& columnBinary,
                                         const std::vector<BSONElement>& expected) {
        BSONColumn col(columnBinary.buf(), columnBinary.len());

        auto it = col.begin();
        for (auto elem : expected) {
            BSONElement other = *it;
            ASSERT(elem.binaryEqualValues(other));
            ASSERT_TRUE(it.more());
            ++it;
        }
    }

    /**
     * A simple path that traverses an object for a set of fields that make up a path.
     */
    struct TestPath {
        std::vector<const char*> elementsToMaterialize(BSONObj refObj) {
            if (_fields.empty()) {
                return {refObj.objdata()};
            }

            BSONObj obj = refObj;
            for (auto iter = _fields.begin(); iter != _fields.end();) {
                auto elem = obj[*iter];
                iter++;
                if (elem.eoo()) {
                    return {};
                }
                if (iter == _fields.end()) {
                    return {elem.value()};
                }
                if (elem.type() != BSONType::object) {
                    return {};
                }
                obj = elem.Obj();
            }

            return {};
        }

        const std::vector<std::string> _fields;
    };

    static void verifyDecompressPathFast(BSONBinData columnBinary,
                                         const std::vector<BSONElement>& expected,
                                         TestPath path) {
        std::vector<TestPath> testPaths{path};
        verifyDecompressPathFast(columnBinary, expected, testPaths);
    }

    static void verifyDecompressPathFast(const BufBuilder& columnBinary,
                                         const std::vector<BSONElement>& expected,
                                         std::span<TestPath> paths) {
        BSONBinData bsonBinData;
        bsonBinData.data = columnBinary.buf();
        bsonBinData.length = columnBinary.len();
        bsonBinData.type = Column;
        verifyDecompressPathFast(bsonBinData, expected, paths);
    }

    static void verifyDecompressPathFast(BSONBinData columnBinary,
                                         const std::vector<BSONElement>& expected,
                                         std::span<TestPath> paths) {
        std::vector<std::vector<BSONElement>> vecs;
        std::vector<std::pair<TestPath, std::vector<BSONElement>&>> testPaths;
        vecs.reserve(paths.size());
        testPaths.reserve(paths.size());
        for (const auto& p : paths) {
            vecs.emplace_back();
            testPaths.emplace_back(p, vecs.back());
        }

        boost::intrusive_ptr allocator{new BSONElementStorage()};
        BSONColumnBlockBased c((const char*)columnBinary.data, columnBinary.length);

        c.decompress<BSONElementMaterializer>(allocator, std::span(testPaths));

        for (auto&& vec : vecs) {
            ASSERT_EQ(vec.size(), expected.size());
        }

        // Each result is a BSONElement at the end of a path
        // Each expected is a root level object
        // Check result matches where path points to in expected
        size_t pathIdx = 0;
        for (auto&& path : paths) {
            auto&& vec = vecs[pathIdx];
            for (size_t i = 0; i < vec.size(); ++i) {
                if (expected[i].eoo()) {
                    ASSERT_TRUE(vec[i].eoo());
                    continue;
                }

                BSONObj obj = expected[i].Obj();
                for (auto iter = path._fields.begin(); iter != path._fields.end();) {
                    auto elem = obj[*iter];
                    iter++;
                    if (elem.eoo()) {
                        // Path failed to resolve in expected, result should be missing
                        ASSERT_TRUE(vec[i].eoo());
                        break;
                    }
                    if (iter == path._fields.end()) {
                        // Path resolved in expected, result should match
                        ASSERT_TRUE(vec[i].binaryEqualValues(elem));
                    } else {
                        // Path is ongoing, expected should not have found a leaf
                        ASSERT(elem.isABSONObj());
                        obj = elem.Obj();
                    }
                }
            }
            ++pathIdx;
        }
    }

    static void verifyDecompression(const BufBuilder& columnBinary,
                                    const std::vector<BSONElement>& expected,
                                    bool testPathDecompression = true) {
        BSONBinData bsonBinData;
        bsonBinData.data = columnBinary.buf();
        bsonBinData.length = columnBinary.len();
        bsonBinData.type = Column;
        verifyDecompression(bsonBinData, expected, testPathDecompression);
    }

    // TODO SERVER-90169 remove 'testPathDecompression'.
    static void verifyDecompression(BSONBinData columnBinary,
                                    const std::vector<BSONElement>& expected,
                                    bool testPathDecompression = true) {
        BSONObjBuilder obj;
        obj.append(""_sd, columnBinary);
        BSONElement columnElement = obj.done().firstElement();

        // Verify that we can traverse BSONColumn twice and extract values on the second pass
        {
            BSONColumn col(columnElement);
            ASSERT_EQ(col.size(), expected.size());
            ASSERT_EQ(std::distance(col.begin(), col.end()), expected.size());
            ASSERT_EQ(col.size(), expected.size());

            auto it = col.begin();
            for (auto elem : expected) {
                BSONElement other = *it;
                ASSERT(elem.binaryEqualValues(other));
                ASSERT_TRUE(it.more());
                ++it;
            }
            ASSERT_FALSE(it.more());
        }

        // Verify that we can traverse BSONColumn and extract values on the first pass
        {
            BSONColumn col(columnElement);

            auto it = col.begin();
            for (auto elem : expected) {
                BSONElement other = *it;
                ASSERT(elem.binaryEqualValues(other));
                ++it;
            }
        }

        // Only run tests with quadratic complexity if size is limited
        if (expected.size() <= 2000) {
            // Verify operator[] when accessing in order
            {
                BSONColumn col(columnElement);

                for (size_t i = 0; i < expected.size(); ++i) {
                    ASSERT(expected[i].binaryEqualValues(*col[i]));
                }
            }

            // Verify operator[] when accessing in reverse order
            {
                BSONColumn col(columnElement);

                for (int i = static_cast<int>(expected.size()) - 1; i >= 0; --i) {
                    ASSERT(expected[i].binaryEqualValues(*col[i]));
                }
            }

            // Verify that we can continue traverse with new iterators when we stop before end
            {
                BSONColumn col(columnElement);

                for (size_t e = 0; e < expected.size(); ++e) {
                    auto it = col.begin();
                    for (size_t i = 0; i < e; ++i, ++it) {
                        ASSERT(expected[i].binaryEqualValues(*it));
                    }
                    ASSERT_EQ(col.size(), expected.size());
                }
            }

            // Verify that we can have multiple iterators on the same thread
            {
                BSONColumn col(columnElement);

                auto it1 = col.begin();
                auto it2 = col.begin();
                auto itEnd = col.end();

                for (; it1 != itEnd && it2 != itEnd; ++it1, ++it2) {
                    ASSERT(it1->binaryEqualValues(*it2));
                }

                ASSERT(it1 == it2);
            }

            // Verify iterator equality operator
            {
                BSONColumn col(columnElement);

                auto iIt = col.begin();
                for (size_t i = 0; i < expected.size(); ++i, ++iIt) {
                    auto jIt = col.begin();
                    for (size_t j = 0; j < expected.size(); ++j, ++jIt) {
                        if (i == j) {
                            ASSERT(iIt == jIt);
                        } else {
                            ASSERT(iIt != jIt);
                        }
                    }
                }
            }
        }

        // Verify we can decompress the entire column using the block-based API using the
        // BSONElementMaterializer.
        {
            bsoncolumn::BSONColumnBlockBased col(columnBinary);
            boost::intrusive_ptr allocator{new BSONElementStorage()};
            std::vector<BSONElement> container;
            col.decompressIterative<BSONElementMaterializer>(container, allocator);
            ASSERT_EQ(container.size(), expected.size());
            auto actual = container.begin();
            BSONElement actualFirst;
            BSONElement actualLast;
            BSONElement actualMin;
            BSONElement actualMax;
            for (auto&& elem : expected) {
                elem.binaryEqualValues(*actual);
                ++actual;
                if (actualFirst.eoo()) {
                    actualFirst = elem;
                }
                if (!elem.eoo()) {
                    actualLast = elem;
                }

                if (actualMin.eoo()) {
                    actualMin = elem;
                    actualMax = elem;
                } else if (!elem.eoo()) {
                    if (elem.woCompare(actualMin) < 0) {
                        actualMin = elem;
                    }
                    if (elem.woCompare(actualMax) > 0) {
                        actualMax = elem;
                    }
                }
            }

            BSONElement firstElem = first<BSONElementMaterializer>(columnBinary, allocator);
            ASSERT_TRUE(firstElem.binaryEqualValues(actualFirst));

            BSONElement lastElem = last<BSONElementMaterializer>(columnBinary, allocator);
            ASSERT_TRUE(lastElem.binaryEqualValues(actualLast));

            BSONElement minElem = min<BSONElementMaterializer>(columnBinary, allocator);
            ASSERT_TRUE(minElem.binaryEqualValues(actualMin));

            BSONElement maxElem = max<BSONElementMaterializer>(columnBinary, allocator);
            ASSERT_TRUE(maxElem.binaryEqualValues(actualMax));

            auto minmaxElems = minmax<BSONElementMaterializer>(columnBinary, allocator);
            ASSERT_TRUE(minmaxElems.first.binaryEqualValues(actualMin));
            ASSERT_TRUE(minmaxElems.second.binaryEqualValues(actualMax));
        }

        // Verify we can decompress the entire column using the block-based API using the
        // SBEColumnMaterializer.
        {
            using SBEMaterializer = sbe::bsoncolumn::SBEColumnMaterializer;
            bsoncolumn::BSONColumnBlockBased col(columnBinary);
            boost::intrusive_ptr allocator{new BSONElementStorage()};
            std::vector<SBEMaterializer::Element> container;
            col.decompressIterative<SBEMaterializer>(container, allocator);
            ASSERT_EQ(container.size(), expected.size());
            auto actual = container.begin();
            for (auto&& elem : expected) {
                convertAndAssertSBEEquals(*actual, elem);
                ++actual;
            }
        }

        {
            boost::intrusive_ptr allocator{new BSONElementStorage()};
            std::vector<BSONElement> collection;
            BSONColumnBlockBased c((const char*)columnBinary.data, columnBinary.length);

            c.decompress<BSONElementMaterializer, std::vector<BSONElement>>(collection, allocator);
            ASSERT_EQ(collection.size(), expected.size());
            for (size_t i = 0; i < collection.size(); ++i) {
                ASSERT(expected[i].binaryEqualValues(collection[i]));
            }
        }

        // Validate that interleaved data returns the expected result in the iterative, block
        // general and block path API.
        verifyDecompressionInterleaved(expected, testPathDecompression);
    }

    /**
     * Constructs a BSON Column encoding with a non-zero delta after the specified element, and
     * expects error 6785500 to be thrown.
     */
    void testInvalidDelta(BSONElement elem) {
        BufBuilder expected;
        appendLiteral(expected, elem);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlock64(expected, Simple8bTypeUtil::encodeInt64(1));
        appendEOO(expected);

        BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
        ASSERT_THROWS_CODE(std::distance(col.begin(), col.end()), DBException, 6785500);
    }

    const boost::optional<uint64_t> kDeltaForBinaryEqualValues = Simple8bTypeUtil::encodeInt64(0);
    const boost::optional<uint128_t> kDeltaForBinaryEqualValues128 =
        Simple8bTypeUtil::encodeInt128(0);

protected:
    struct TrackingContextChecker {
        ~TrackingContextChecker() {
            // Ensure we have freed all memory we allocated and are tracking this properly.
            ASSERT_EQ(trackingContext.allocated(), 0);
        }

        tracking::Context trackingContext;
    };

    // Needs to be defined first so it is destroyed after BSONColumnBuilder
    TrackingContextChecker trackingContextChecker;
    BSONColumnBuilder<tracking::Allocator<void>> cb{
        trackingContextChecker.trackingContext.makeAllocator<void>()};

private:
    std::forward_list<BSONObj> _elementMemory;
};

TEST_F(BSONColumnTest, FuzzerDiscoveredEdgeCases) {
    // This test is a collection of binaries produced by the fuzzer that exposed bugs at some point
    // and contains coverage missing from the tests defined above.
    std::vector<StringData> binariesBase64 = {
        // Ends with uncompressed literal. Last value in previous block needs to be set correctly
        // for doubles.
        "AQAACQgAAHMA7wkAQP/Q0CfU0NCACvX//////9AA"_sd,
        // Contains zero deltas after uncompressed string starting with '\0' (unencodable). Ensures
        // we have special handling for zero deltas that by-pass materialization.
        "CAAAAgACAAAAAACAAgAAAAAAAAAA"_sd,
        // Re-scaling double is not possible. Offset to last control byte needs to be cleared so a
        // new control byte is written by the compressor.
        "AQAAAAAAAAAAAJHCgLGRkf//DZGRCJEACAAAgDqRsZGRkZGRAA=="_sd,
        // Re-scaling double is not possible. Offset to last control byte needs to be cleared so a
        // new control byte is written by the compressor.
        "CgABAP//////////gAIBAAD7///4AA=="_sd,
        // Ends with value too large to be encodable in Simple8b block
        "AQAAAAAjAAAAHAkALV3DRTINAACAd/ce/////xwJAC33Hv////+/AA=="_sd,
        // Unencodable literal for 128bit types, prevEncoded128 needs to be set to none by
        // compressor.
        "DQAUAAAAAAgAAIDx///++AAAAAMAAAAIAACA8f///vj/AAAA"_sd,
        // Merge of interleaved objects that results in repeated fieldname
        "fwB/APEPAAAA/wD/////KwAGAAALAJ0qnZ0AAICx87tAc/+/fgQABwAAAP8AAICx88BEjAi/AICxAAIAACQAFgAA"_sd};

    for (auto&& binaryBase64 : binariesBase64) {
        auto binary = base64::decode(binaryBase64);

        // Validate that our BSONColumnBuilder would construct this exact binary for the input data.
        // This is required to be able to verify these binary blobs.
        BSONColumnBuilder builder;
        BSONColumn column(binary.data(), binary.size());
        try {
            for (auto&& elem : column) {
                builder.append(elem);
            }
        } catch (const DBException& ex) {
            ASSERT_NOT_OK(ex.toStatus());
            continue;
        }
        BSONBinData finalized = builder.finalize();
        ASSERT_EQ(binary.size(), finalized.length);
        ASSERT(memcmp(binary.data(), finalized.data, binary.size()) == 0);

        // Validate that reopening BSONColumnBuilder from this binary produces the same state as-if
        // values were uncompressed and re-appended.
        verifyColumnReopenFromBinary(binary.data(), binary.size());
    }
}

TEST_F(BSONColumnTest, BlockFuzzerDiscoveredEdgeCases) {
    // This test is a collection of binaries produced by the decompression fuzzer that exposed bugs
    // in the block-based or iterator API, and contains coverage missing from the tests defined
    // above. This test validates that the iterator API and the block-based API must produce the
    // same results.
    std::vector<StringData> binariesBase64 = {
        // Iterator API did not cast values to booleans before materializing (SERVER-87779).
        "CAAAoJb//wD/3ylEAA=="_sd,
        // Block-based API updated the 'lastValue' when appending EOO elements (SERVER-85860).
        "CgAKAAoAEwAHAAoACgEAAABQUFBQUFBQUFAAAAAAAACoqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioUFBQUAAAAAAACgAKAAsAEwAKAAoACgAKAAoACgAKAAoACgAKAAoACgAA"_sd,
        // Block-based API didn't validate the scale index for simple8b blocks (SERVER-87628 and
        // SERVER-88738).
        "QAEADP////+SAA=="_sd,
        "fwBAAwAAAAAAAAAA"_sd,
        "CgBh/wABemEUAAAAAAAAAAIBAAA="_sd,
        "BQAvAAAAAABQslBQUFBQUFBQUFAAUFBQUFB5UP7///9QUFBQUFBQUFCBgYGBgYGBgYGBgYGBgYFQbFCpUFBQgVBQUFBQUFBQP1BQUFBQUAAA"_sd,
        // Block-based Path API doesn't validate the scale index for non-double values
        // (SERVER-89155).
        "8AgAAAAIAAAA0Cz/AAAAAAdSAAA="_sd,
        // The two APIs had different delta values, but both should fail (SERVER-85860 and
        // SERVER-87873).
        "BQADAAAAkP8AkJCR///+/4jIfdAmAAAAAAAAAJACAAAAAP8AAAA="_sd,
        "fwDQYG9tfwAAAAAA"_sd,
        "CAABwMABwMDAwMDAwH9DwMDAwMDAwMDAwMDAwMDAwMjAwMDAAAAAAA=="_sd,
        "EwAAAGCvYK+vUgBSUlBQc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3NzFBQUFBQUc3PQ0NDQ0NDQ0NDQr1JSUlBQ0NDQ0NDQ0NDQ0NIYAAAA0NAXlaJ9//8AAA=="_sd,
        // Block-based API using the table decoders should fail on bad selectors (SERVER-88062).
        "CwBPpFpaWloAAKD3Af9dXQD/AAA="_sd,
        // Block-based API had a stack overflow for BinData values (SERVER-88207).
        "BQAXAAAAMcLCPso9PcJhJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmsMIYAAECAAIAAA=="_sd,
        "BQAwAAAAAAcAAAAAAAEAAAAAAABAAAAAAAA7Ozs7Ozs7Ozs6Ozs7Ozs7Ozs7Ozs7Ozs7OwD+/4A7OzsA/v+A/wA="_sd,
        // Block-based API didn't allow non-zero/missing deltas after EOO (SERVER-89150).
        "8h4AAAD/p/+zSENBMoAB/0hDQzKAAP9IOjCAAP8AAACCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCggA="_sd,
        // Blockbased API didn't update last to EOO when Iterative API did for interleaved data
        // (SERVER-89612).
        "8hQAAAAF+P//////FCgAAAAAAAAABgAIAACBKg7/+///////MP8V/3EAAACBeHFYDAAA/3RhZ3P//wEAAAA="_sd,
        // Blockbased API doesn't fail an interleaved mode that has leftover data in some decoders
        // while the iterative version does (SERVER-92150)
        "8jIAAAAHVvkCAAEAAAAAAgABAAAAAAxTdGNydHVfaWQAAQAAAAABMg5faWQAAQAAAAAA/wD/AP8Aj4+Pj4+Pj4+Pj4+Pj4+Pj4//AP8A/wD/AP8A/wD/AP8A/wCPj4+Pj4+Pj4+PAAD/AP8A/wD/AP8A/wCPj4+Pj4+Pj49vj4+Pj4+Pj/8A/041j4+Pj4+Pj4+Pj48BAFXeV6t2AI+Pj4+Pj4+Pj4+Pj8SPj4+Pj4+Pj4//AP8A/wD/AP8A/wAA"_sd,
        // Empty interleaved mode produces an assert in block-based API but not iterative
        // (SERVER-92327)
        "EAAAYTsB8gcAAAD/AAAAgsj//////////wEH//hB/7KyAP+AAP//AAA="_sd,
    };

    for (auto&& binaryBase64 : binariesBase64) {
        auto binary = base64::decode(binaryBase64);

        // Store the results for validation after decompression.
        boost::intrusive_ptr allocator{new BSONElementStorage()};
        std::vector<BSONElement> iteratorElems, blockBasedElems;
        bool blockBasedError = false;
        bool iteratorError = false;

        // Attempt to decompress using the block-based API.
        bsoncolumn::BSONColumnBlockBased block(binary.data(), binary.size());
        try {
            block.decompress<bsoncolumn::BSONElementMaterializer, std::vector<BSONElement>>(
                blockBasedElems, allocator);
        } catch (...) {
            blockBasedError = true;
        }

        // Attempt to decompress using the iterator API.
        BSONColumn column(binary.data(), binary.size());
        try {
            for (auto&& elem : column) {
                iteratorElems.push_back(elem);
            };
        } catch (...) {
            iteratorError = true;
        }

        // If one API failed, then both APIs must fail.
        if (iteratorError || blockBasedError) {
            ASSERT(iteratorError && blockBasedError);
            continue;
        }

        // If the APIs succeeded, the results must be the same.
        ASSERT(iteratorElems.size() == blockBasedElems.size());
        auto it = iteratorElems.begin();
        for (auto&& elem : blockBasedElems) {
            ASSERT(elem.binaryEqualValues(*it));
            ++it;
        }
    }
}

TEST_F(BSONColumnTest, BuilderFuzzerGenerationDiscoveredEdgeCases) {
    // This test is a collection of binaries produced by the builder fuzzer that exposed bugs
    // in the bsoncolumn builder.  To investigate new fuzzer failures, find the base64 encoding
    // of the fuzzer string in the logs (not to be confused with the column binary itself) and
    // add it to the binaries vector
    //
    // This should look like
    //
    // Base64:
    // <base64 string>
    //
    std::vector<StringData> binariesBase64 = {};

    for (auto&& binaryBase64 : binariesBase64) {
        auto binary = base64::decode(binaryBase64);

        std::forward_list<BSONObj> elementMemory;
        std::vector<BSONElement> generatedElements;

        // Generate elements from input data
        const char* ptr = binary.data();
        const char* end = binary.data() + binary.size();
        while (ptr < end) {
            BSONElement element;
            int repetition;
            if (!mongo::bsoncolumn::createFuzzedElement(
                    ptr, end, elementMemory, repetition, element)) {
                generatedElements.clear();  // Bad input string to element generation
                break;
            }
            if (!bsoncolumn::addFuzzedElements(
                    ptr, end, elementMemory, element, repetition, generatedElements)) {
                generatedElements.clear();  // Bad input string to run generation
                break;
            }
        }
        if (generatedElements.empty())
            continue;

        // Exercise the builder
        BSONColumnBuilder builder;
        for (auto element : generatedElements) {
            builder.append(element);
        }

        // Verify decoding gives us original elements
        auto diff = builder.intermediate();
        BSONColumn col(diff.data(), diff.size());
        auto it = col.begin();
        for (auto elem : generatedElements) {
            BSONElement other = *it;
            ASSERT_TRUE(elem.binaryEqualValues(other));
            ASSERT_TRUE(it.more());
            ++it;
        }
        ASSERT_TRUE(!it.more());
    }
}

TEST_F(BSONColumnTest, BuilderFuzzerReopenDiscoveredEdgeCases) {
    // This test is a collection of binaries produced by the builder fuzzer that exposed bugs
    // in the bsoncolumn builder outputs.  To investigate new fuzzer failures, find the base64
    // encoding of the column binary in the logs (not to be confused with the fuzzer string) and add
    // it to the binaries vector
    //
    // This should look like
    //
    // Column: <base64 string>
    //
    std::vector<StringData> binariesBase64 = {
        // Pending fix of SERVER-100659
        //        "gPz/////////CAAAgP7/////////AQAAAAAAAAAAYI/OxcXFxcXFAQ4AAAAAAAAB7uLi4uLi4gAuHR0dHR2dAI5xcXFxcXEAjnFxcXFxcQCOcXFxcXFxAK6rq6urq2sAzri4uLi4OADOuLi4uLg4AM64uLi4uDgAzri4uLi4OADOuLi4uLg4AM64uLi4uDgAzri4uLi4OADOuLi4uLg4AI9ulpaWlpY2AG5cXFxcXBwAblxcXFxcHABuXFxcXFwcAG5cXFxcXBwAblxcXFxcHABuXFxcXFwcAG5cXFxcXBwAblxcXFxcHABuXFxcXFwcAG5cXFxcXBwAblxcXFxcHABuXFxcXFwcAG5cXFxcXBwAblxcXFxcHABuXFxcXFwcAI9uXFxcXFwcAG5cXFxcXBwA7gsMDAwMHAAuLi4uLi4OAC4uLi4uLg4ALi4uLi4uDgAuLi4uLi4OAC4uLi4uLg4ALi4uLi4uDgAuLi4uLi4OAC4uLi4uLg4ALi4uLi4uDgAuLi4uLi4OAC4uLi4uLg4ALi4uLi4uDgAuLi4uLi4OAI8uLi4uLi4OAC4uLi4uLg4ALi4uLi4uDgAuLi4uLi4OAC4uLi4uLg4ALi4uLi4uDgAuLi4uLi4OAC4uLi4uLg4ALi4uLi4uDgAuLi4uLi4OAC4uLi4uLg4ALi4uLi4uDgAuLi4uLi4OAC4uLi4uLg4ALi4uLi4uDgAuLi4uLi4OAI8uLi4uLi4OAC4uLi4uLg4ALi4uLi4uDgAuLi4uLi4OAC4uLi4uLg4ALi4uLi4uDgAuLi4uLi4OAK6wr6+vrwcALhcXFxcXBwAuFxcXFxcHAC4XFxcXFwcALhcXFxcXBwAuFxcXFxcHAC4XFxcXFwcALhcXFxcXBwAuFxcXFxcHAI8uFxcXFxcHAC4XFxcXFwcALhcXFxcXBwAuFxcXFxcHAC4XFxcXFwcALhcXFxcXBwAuFxcXFxcHAC4XFxcXFwcALhcXFxcXBwAuFxcXFxcHAC4XFxcXFwcALhcXFxcXBwAuFxcXFxcHAC4XFxcXFwcALhcXFxcXBwAuFxcXFxcHAIYuFxcXFxcHAC4XFxcXFwcALhcXFxcXBwAuFxcXFxcHAC4XFxcXFwcALhcXFxcXBwAuFxcXFxcHAAA="_sd,
    };

    for (auto&& binaryBase64 : binariesBase64) {
        auto binary = base64::decode(binaryBase64);

        BSONColumnBuilder builder;
        BSONColumn col(binary.data(), binary.size());
        for (auto&& elem : col) {
            builder.append(elem);
        }

        [[maybe_unused]] auto d = builder.intermediate();

        // Verify binary reopen gives identical state as intermediate
        BSONColumnBuilder reopen(binary.data(), binary.size());
        ASSERT_TRUE(builder.isInternalStateIdentical(reopen));
    }
}

TEST_F(BSONColumnTest, Empty) {
    BufBuilder expected;
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {});
}

TEST_F(BSONColumnTest, ContainsScalarInt32SimpleCompressed) {
    // Column should have several scalar values of same type
    // -> 32-bit Ints
    auto elemInt32_0 = createElementInt32(100);
    auto elemInt32_1 = createElementInt32(101);
    cb.append(elemInt32_0);
    cb.append(elemInt32_0);
    cb.append(elemInt32_1);
    auto binData = cb.finalize();

    // Recreate cb "manually" to create the BSONColumn class, so as to
    // test BSONColumn::contains_forTest
    BufBuilder colBuf;
    appendLiteral(colBuf, elemInt32_0);
    appendSimple8bControl(colBuf, 0b1000, 0b0000);  // Control = 1, CountOfSimple8b's = 0+1
    std::vector<boost::optional<uint64_t>> v{deltaInt32(elemInt32_0, elemInt32_0),
                                             deltaInt32(elemInt32_1, elemInt32_0)};
    appendSimple8bBlocks64(colBuf, v, 1);
    appendEOO(colBuf);
    BSONColumn col(createBSONColumn(colBuf.buf(), colBuf.len()));

    ASSERT_EQ(col.contains_forTest(BSONType::numberInt), true);
    ASSERT_EQ(col.contains_forTest(BSONType::numberLong), false);
    ASSERT_EQ(col.contains_forTest(BSONType::numberDouble), false);
    ASSERT_EQ(col.contains_forTest(BSONType::array), false);
    ASSERT_EQ(col.contains_forTest(BSONType::timestamp), false);
    ASSERT_EQ(col.contains_forTest(BSONType::string), false);
    ASSERT_EQ(col.contains_forTest(BSONType::object), false);
    ASSERT_EQ(col.contains_forTest(BSONType::oid), false);
    ASSERT_EQ(col.contains_forTest(BSONType::boolean), false);

    verifyBinary(binData, colBuf);
}

TEST_F(BSONColumnTest, ContainsScalarInt64SimpleCompressed) {
    // Column should have several scalar values of same type
    // -> 64-bit Ints
    auto elemInt64_0 = createElementInt64(100);
    auto elemInt64_1 = createElementInt64(101);
    cb.append(elemInt64_0);
    cb.append(elemInt64_0);
    cb.append(elemInt64_1);
    auto binData = cb.finalize();

    // Recreate cb "manually" to create the BSONColumn class, so as to
    // test BSONColumn::contains_forTest
    BufBuilder colBuf;
    appendLiteral(colBuf, elemInt64_0);
    appendSimple8bControl(colBuf, 0b1000, 0b0000);  // Control = 1, CountOfSimple8b's = 0+1
    std::vector<boost::optional<uint64_t>> v{deltaInt64(elemInt64_0, elemInt64_0),
                                             deltaInt64(elemInt64_1, elemInt64_0)};
    appendSimple8bBlocks64(colBuf, v, 1);
    appendEOO(colBuf);
    BSONColumn col(createBSONColumn(colBuf.buf(), colBuf.len()));

    ASSERT_EQ(col.contains_forTest(BSONType::numberInt), false);
    ASSERT_EQ(col.contains_forTest(BSONType::numberLong), true);
    ASSERT_EQ(col.contains_forTest(BSONType::numberDouble), false);
    ASSERT_EQ(col.contains_forTest(BSONType::array), false);
    ASSERT_EQ(col.contains_forTest(BSONType::timestamp), false);
    ASSERT_EQ(col.contains_forTest(BSONType::string), false);
    ASSERT_EQ(col.contains_forTest(BSONType::object), false);
    ASSERT_EQ(col.contains_forTest(BSONType::oid), false);
    ASSERT_EQ(col.contains_forTest(BSONType::boolean), false);

    verifyBinary(binData, colBuf);
}

TEST_F(BSONColumnTest, ContainsScalarDoubleSimpleCompressed) {
    // Column should have several scalar values of same type
    // -> Double
    auto elemDouble_0 = createElementDouble(100);
    auto elemDouble_1 = createElementDouble(101);
    cb.append(elemDouble_0);
    cb.append(elemDouble_0);
    cb.append(elemDouble_1);
    auto binData = cb.finalize();

    // Recreate cb "manually" to create the BSONColumn class, so as to
    // test BSONColumn::contains_forTest
    BufBuilder colBuf;
    appendLiteral(colBuf, elemDouble_0);
    appendSimple8bControl(
        colBuf, 0b1001, 0b0000);  // Control = 1001 (double only, scale=0), CountOfSimple8b's = 0+1
    std::vector<boost::optional<uint64_t>> v{deltaDouble(elemDouble_0, elemDouble_0, 1),
                                             deltaDouble(elemDouble_1, elemDouble_0, 1)};
    appendSimple8bBlocks64(colBuf, v, 1);
    appendEOO(colBuf);
    BSONColumn col(createBSONColumn(colBuf.buf(), colBuf.len()));

    ASSERT_EQ(col.contains_forTest(BSONType::numberInt), false);
    ASSERT_EQ(col.contains_forTest(BSONType::numberLong), false);
    ASSERT_EQ(col.contains_forTest(BSONType::numberDouble), true);
    ASSERT_EQ(col.contains_forTest(BSONType::array), false);
    ASSERT_EQ(col.contains_forTest(BSONType::timestamp), false);
    ASSERT_EQ(col.contains_forTest(BSONType::string), false);
    ASSERT_EQ(col.contains_forTest(BSONType::object), false);
    ASSERT_EQ(col.contains_forTest(BSONType::oid), false);
    ASSERT_EQ(col.contains_forTest(BSONType::boolean), false);

    verifyBinary(binData, colBuf);
}

TEST_F(BSONColumnTest, ContainsScalarTimestampSimpleCompressed) {
    // Column should have several scalar values of same type
    // -> Timestamp
    auto elemTimestamp_0 = createTimestamp(Timestamp(0));
    auto elemTimestamp_1 = createTimestamp(Timestamp(1));
    cb.append(elemTimestamp_0);
    cb.append(elemTimestamp_0);
    cb.append(elemTimestamp_1);
    auto binData = cb.finalize();

    // Recreate cb "manually" to create the BSONColumn class, so as to
    // test BSONColumn::contains_forTest
    BufBuilder colBuf;
    appendLiteral(colBuf, elemTimestamp_0);
    appendSimple8bControl(colBuf, 0b1000, 0b0000);
    std::vector<boost::optional<uint64_t>> expectedDeltaOfDeltas{
        deltaOfDeltaTimestamp(elemTimestamp_0, elemTimestamp_0),
        deltaOfDeltaTimestamp(elemTimestamp_1, elemTimestamp_0)};
    appendSimple8bBlocks64(colBuf, expectedDeltaOfDeltas, 1);
    appendEOO(colBuf);
    BSONColumn col(createBSONColumn(colBuf.buf(), colBuf.len()));

    ASSERT_EQ(col.contains_forTest(BSONType::numberInt), false);
    ASSERT_EQ(col.contains_forTest(BSONType::numberLong), false);
    ASSERT_EQ(col.contains_forTest(BSONType::numberDouble), false);
    ASSERT_EQ(col.contains_forTest(BSONType::array), false);
    ASSERT_EQ(col.contains_forTest(BSONType::timestamp), true);
    ASSERT_EQ(col.contains_forTest(BSONType::string), false);
    ASSERT_EQ(col.contains_forTest(BSONType::object), false);
    ASSERT_EQ(col.contains_forTest(BSONType::oid), false);
    ASSERT_EQ(col.contains_forTest(BSONType::boolean), false);

    verifyBinary(binData, colBuf);
}

TEST_F(BSONColumnTest, ContainsScalarStringSimpleCompressed) {
    // Column should have several scalar values of same type
    // -> String
    auto elemString_0 = createElementString("hello");
    auto elemString_1 = createElementString("hellp");
    cb.append(elemString_0);
    cb.append(elemString_1);
    auto binData = cb.finalize();

    // Recreate cb "manually" to create the BSONColumn class, so as to
    // test BSONColumn::contains_forTest
    BufBuilder colBuf;
    appendLiteral(colBuf, elemString_0);
    appendSimple8bControl(colBuf, 0b1000, 0b0000);  // Control = 1, CountOfSimple8b's = 0+1
    appendSimple8bBlock128(colBuf, deltaString(elemString_1, elemString_0));
    appendEOO(colBuf);
    BSONColumn col(createBSONColumn(colBuf.buf(), colBuf.len()));

    ASSERT_EQ(col.contains_forTest(BSONType::numberInt), false);
    ASSERT_EQ(col.contains_forTest(BSONType::numberLong), false);
    ASSERT_EQ(col.contains_forTest(BSONType::numberDouble), false);
    ASSERT_EQ(col.contains_forTest(BSONType::array), false);
    ASSERT_EQ(col.contains_forTest(BSONType::timestamp), false);
    ASSERT_EQ(col.contains_forTest(BSONType::string), true);
    ASSERT_EQ(col.contains_forTest(BSONType::object), false);
    ASSERT_EQ(col.contains_forTest(BSONType::oid), false);
    ASSERT_EQ(col.contains_forTest(BSONType::boolean), false);

    verifyBinary(binData, colBuf);
}

TEST_F(BSONColumnTest, ContainsScalarObjectIDSimpleCompressed) {
    // Column should have several scalar values of same type
    // -> ObjectID
    auto elemObjectID_0 = createObjectId(OID("112233445566778899AABBCC"));
    auto elemObjectID_1 = createObjectId(OID("112233445566778899AABBCD"));
    cb.append(elemObjectID_0);
    cb.append(elemObjectID_0);
    cb.append(elemObjectID_1);
    auto binData = cb.finalize();

    // Recreate cb "manually" to create the BSONColumn class, so as to
    // test BSONColumn::contains_forTest
    BufBuilder colBuf;
    appendLiteral(colBuf, elemObjectID_0);
    appendSimple8bControl(colBuf, 0b1000, 0b0000);  // Control = 1, CountOfSimple8b's = 0+1

    std::vector<boost::optional<uint64_t>> v{
        // Don't encode first value.
        // For second value, set prevprev to prev.
        // => (val, prev, prev)
        deltaOfDeltaObjectId(elemObjectID_0, elemObjectID_0, elemObjectID_0),
        // steady state => (val, prev, prevprev)
        deltaOfDeltaObjectId(elemObjectID_1, elemObjectID_0, elemObjectID_0)};
    appendSimple8bBlocks64(colBuf, v, 1);
    appendEOO(colBuf);
    BSONColumn col(createBSONColumn(colBuf.buf(), colBuf.len()));

    ASSERT_EQ(col.contains_forTest(BSONType::numberInt), false);
    ASSERT_EQ(col.contains_forTest(BSONType::numberLong), false);
    ASSERT_EQ(col.contains_forTest(BSONType::numberDouble), false);
    ASSERT_EQ(col.contains_forTest(BSONType::array), false);
    ASSERT_EQ(col.contains_forTest(BSONType::timestamp), false);
    ASSERT_EQ(col.contains_forTest(BSONType::string), false);
    ASSERT_EQ(col.contains_forTest(BSONType::object), false);
    ASSERT_EQ(col.contains_forTest(BSONType::oid), true);
    ASSERT_EQ(col.contains_forTest(BSONType::boolean), false);

    verifyBinary(binData, colBuf);
}

TEST_F(BSONColumnTest, ContainsScalarBoolSimpleCompressed) {
    // Column should have several scalar values of same type
    // -> Bool
    auto elemBool_0 = createBool(true);
    auto elemBool_1 = createBool(false);
    cb.append(elemBool_0);
    cb.append(elemBool_0);
    cb.append(elemBool_1);
    auto binData = cb.finalize();

    // Recreate cb "manually" to create the BSONColumn class, so as to
    // test BSONColumn::contains_forTest
    BufBuilder colBuf;
    appendLiteral(colBuf, elemBool_0);
    appendSimple8bControl(colBuf, 0b1000, 0b0000);  // Control = 1, CountOfSimple8b's = 0+1
    std::vector<boost::optional<uint64_t>> v{deltaBool(elemBool_0, elemBool_0),
                                             deltaBool(elemBool_1, elemBool_0)};
    appendSimple8bBlocks64(colBuf, v, 1);
    appendEOO(colBuf);
    BSONColumn col(createBSONColumn(colBuf.buf(), colBuf.len()));

    ASSERT_EQ(col.contains_forTest(BSONType::numberInt), false);
    ASSERT_EQ(col.contains_forTest(BSONType::numberLong), false);
    ASSERT_EQ(col.contains_forTest(BSONType::numberDouble), false);
    ASSERT_EQ(col.contains_forTest(BSONType::array), false);
    ASSERT_EQ(col.contains_forTest(BSONType::timestamp), false);
    ASSERT_EQ(col.contains_forTest(BSONType::string), false);
    ASSERT_EQ(col.contains_forTest(BSONType::object), false);
    ASSERT_EQ(col.contains_forTest(BSONType::oid), false);
    ASSERT_EQ(col.contains_forTest(BSONType::boolean), true);

    verifyBinary(binData, colBuf);
}

TEST_F(BSONColumnTest, BasicValue) {
    auto elem = createElementInt32(1);
    cb.append(elem);
    cb.append(elem);

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, 0);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elem, elem});
}

TEST_F(BSONColumnTest, BasicSkip) {
    auto elem = createElementInt32(1);
    cb.append(elem);
    cb.skip();

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, boost::none);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elem, BSONElement()});
}

TEST_F(BSONColumnTest, BasicSkipRepeat) {
    auto elem = createElementInt32(1);
    cb.append(elem);
    cb.skip();
    cb.append(elem);

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    std::vector<boost::optional<uint64_t>> expectedDeltas{boost::none, 0};
    appendSimple8bBlocks64(expected, expectedDeltas, 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elem, BSONElement(), elem});
}

TEST_F(BSONColumnTest, OnlySkip) {
    cb.skip();

    BufBuilder expected;
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, boost::none);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {BSONElement()});
}

TEST_F(BSONColumnTest, OnlySkipMany) {
    // This test checks that we can setup the correct RLE state when reopening BSONColumnBuilder in
    // the case that the RLE blocks contain skip.
    for (int i = 0; i < 500; ++i) {
        cb.skip();
    }

    auto binData = cb.finalize();
    verifyColumnReopenFromBinary(reinterpret_cast<const char*>(binData.data), binData.length);
}

TEST_F(BSONColumnTest, ValueAfterSkip) {
    auto elem = createElementInt32(1);
    cb.skip();
    cb.append(elem);

    BufBuilder expected;
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, boost::none);
    appendLiteral(expected, elem);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {BSONElement(), elem});
}

TEST_F(BSONColumnTest, MultipleSimple8bBlocksAfterControl) {
    std::vector<BSONElement> elems;
    for (int i = 0; i < 100; ++i) {
        elems.push_back(createElementInt64(i % 2));
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }


    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0100);
    appendSimple8bBlocks64(expected, deltaInt64(elems.begin() + 1, elems.end(), elems.front()), 5);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, MultipleSimple8bBlocksAfterControl128) {
    std::vector<BSONElement> elems;
    for (int i = 0; i < 100; ++i) {
        // Generate strings from integer to make it easier to control the delta values
        auto str = Simple8bTypeUtil::decodeString(i % 2);
        elems.push_back(createElementString(StringData(str.str.data(), str.size)));
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0100);
    appendSimple8bBlocks128(
        expected, deltaString(elems.begin() + 1, elems.end(), elems.front()), 5);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, MultipleSimple8bBlockRewriteAtEnd) {
    // This data set uncovered an interesting edge case. Appending 75 elements, call intermediate()
    // appending the remaining 20 and call intermediate again. This results in the total binary
    // length not changing in the second intermediate call and the control block from the two
    // intermediate calls being identical. When calculating the offset for the first byte that
    // changed in the second intermediate call we must take care to consider the simple8b blocks
    // that got re-written at the end (to contain more elements).
    std::vector<Date_t> timestamps = {
        Date_t::fromMillisSinceEpoch(1709224256311), Date_t::fromMillisSinceEpoch(1709224256318),
        Date_t::fromMillisSinceEpoch(1709224256326), Date_t::fromMillisSinceEpoch(1709224256333),
        Date_t::fromMillisSinceEpoch(1709224256340), Date_t::fromMillisSinceEpoch(1709224256348),
        Date_t::fromMillisSinceEpoch(1709224256355), Date_t::fromMillisSinceEpoch(1709224256363),
        Date_t::fromMillisSinceEpoch(1709224256370), Date_t::fromMillisSinceEpoch(1709224256378),
        Date_t::fromMillisSinceEpoch(1709224256385), Date_t::fromMillisSinceEpoch(1709224256392),
        Date_t::fromMillisSinceEpoch(1709224256400), Date_t::fromMillisSinceEpoch(1709224256407),
        Date_t::fromMillisSinceEpoch(1709224256415), Date_t::fromMillisSinceEpoch(1709224256422),
        Date_t::fromMillisSinceEpoch(1709224256429), Date_t::fromMillisSinceEpoch(1709224256437),
        Date_t::fromMillisSinceEpoch(1709224256444), Date_t::fromMillisSinceEpoch(1709224256452),
        Date_t::fromMillisSinceEpoch(1709224256459), Date_t::fromMillisSinceEpoch(1709224256466),
        Date_t::fromMillisSinceEpoch(1709224256474), Date_t::fromMillisSinceEpoch(1709224256481),
        Date_t::fromMillisSinceEpoch(1709224256489), Date_t::fromMillisSinceEpoch(1709224256496),
        Date_t::fromMillisSinceEpoch(1709224256504), Date_t::fromMillisSinceEpoch(1709224256511),
        Date_t::fromMillisSinceEpoch(1709224256519), Date_t::fromMillisSinceEpoch(1709224256526),
        Date_t::fromMillisSinceEpoch(1709224256533), Date_t::fromMillisSinceEpoch(1709224256541),
        Date_t::fromMillisSinceEpoch(1709224256548), Date_t::fromMillisSinceEpoch(1709224256556),
        Date_t::fromMillisSinceEpoch(1709224256563), Date_t::fromMillisSinceEpoch(1709224256570),
        Date_t::fromMillisSinceEpoch(1709224256578), Date_t::fromMillisSinceEpoch(1709224256585),
        Date_t::fromMillisSinceEpoch(1709224256592), Date_t::fromMillisSinceEpoch(1709224256600),
        Date_t::fromMillisSinceEpoch(1709224256607), Date_t::fromMillisSinceEpoch(1709224256614),
        Date_t::fromMillisSinceEpoch(1709224256622), Date_t::fromMillisSinceEpoch(1709224256629),
        Date_t::fromMillisSinceEpoch(1709224256636), Date_t::fromMillisSinceEpoch(1709224256644),
        Date_t::fromMillisSinceEpoch(1709224256651), Date_t::fromMillisSinceEpoch(1709224256658),
        Date_t::fromMillisSinceEpoch(1709224256666), Date_t::fromMillisSinceEpoch(1709224256673),
        Date_t::fromMillisSinceEpoch(1709224256681), Date_t::fromMillisSinceEpoch(1709224256688),
        Date_t::fromMillisSinceEpoch(1709224256695), Date_t::fromMillisSinceEpoch(1709224256703),
        Date_t::fromMillisSinceEpoch(1709224256710), Date_t::fromMillisSinceEpoch(1709224256718),
        Date_t::fromMillisSinceEpoch(1709224256725), Date_t::fromMillisSinceEpoch(1709224256733),
        Date_t::fromMillisSinceEpoch(1709224256740), Date_t::fromMillisSinceEpoch(1709224256747),
        Date_t::fromMillisSinceEpoch(1709224256755), Date_t::fromMillisSinceEpoch(1709224256762),
        Date_t::fromMillisSinceEpoch(1709224256769), Date_t::fromMillisSinceEpoch(1709224256777),
        Date_t::fromMillisSinceEpoch(1709224256784), Date_t::fromMillisSinceEpoch(1709224256792),
        Date_t::fromMillisSinceEpoch(1709224256799), Date_t::fromMillisSinceEpoch(1709224256806),
        Date_t::fromMillisSinceEpoch(1709224256814), Date_t::fromMillisSinceEpoch(1709224256821),
        Date_t::fromMillisSinceEpoch(1709224256829), Date_t::fromMillisSinceEpoch(1709224256836),
        Date_t::fromMillisSinceEpoch(1709224256843), Date_t::fromMillisSinceEpoch(1709224256851),
        Date_t::fromMillisSinceEpoch(1709224256858), Date_t::fromMillisSinceEpoch(1709224256866),
        Date_t::fromMillisSinceEpoch(1709224256873), Date_t::fromMillisSinceEpoch(1709224256880),
        Date_t::fromMillisSinceEpoch(1709224256888), Date_t::fromMillisSinceEpoch(1709224256895),
        Date_t::fromMillisSinceEpoch(1709224256903), Date_t::fromMillisSinceEpoch(1709224256910),
        Date_t::fromMillisSinceEpoch(1709224256917), Date_t::fromMillisSinceEpoch(1709224256925),
        Date_t::fromMillisSinceEpoch(1709224256932), Date_t::fromMillisSinceEpoch(1709224256940),
        Date_t::fromMillisSinceEpoch(1709224256947), Date_t::fromMillisSinceEpoch(1709224256954),
        Date_t::fromMillisSinceEpoch(1709224256962), Date_t::fromMillisSinceEpoch(1709224256969),
        Date_t::fromMillisSinceEpoch(1709224256976), Date_t::fromMillisSinceEpoch(1709224256984),
        Date_t::fromMillisSinceEpoch(1709224256991), Date_t::fromMillisSinceEpoch(1709224256999),
        Date_t::fromMillisSinceEpoch(1709224257006),
    };

    std::vector<BSONElement> elems;
    for (auto&& ts : timestamps) {
        elems.push_back(createDate(ts));
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0100);
    appendSimple8bBlocks64(
        expected,
        deltaOfDeltaDates(elems.begin() + 1, elems.end(), elems.front(), elems.front()),
        5);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, LargeDeltaIsLiteral) {
    auto first = createElementInt64(1);
    cb.append(first);

    auto second = createElementInt64(std::numeric_limits<int64_t>::max());
    cb.append(second);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendLiteral(expected, second);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second});
}

TEST_F(BSONColumnTest, LargeDeltaIsLiteralAfterSimple8b) {
    auto zero = createElementInt64(0);
    cb.append(zero);
    cb.append(zero);

    auto large = createElementInt64(std::numeric_limits<int64_t>::max());
    cb.append(large);
    cb.append(large);

    BufBuilder expected;
    appendLiteral(expected, zero);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaInt64(zero, zero));
    appendLiteral(expected, large);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaInt64(large, large));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {zero, zero, large, large});
}

TEST_F(BSONColumnTest, OverBlockCount) {
    std::vector<BSONElement> elems;
    int64_t val = 0xFFFFFFFFFFFF;

    for (int i = 0; i < 20; ++i) {
        elems.push_back(createElementInt64(val));
        val = -val;
    }

    for (const auto& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b1111);

    auto chunk1Begin = elems.begin() + 1;
    auto chunk1End = chunk1Begin + 16;
    appendSimple8bBlocks64(expected, deltaInt64(chunk1Begin, chunk1End, elems.front()), 16);

    appendSimple8bControl(expected, 0b1000, 0b0010);
    appendSimple8bBlocks64(expected, deltaInt64(chunk1End, elems.end(), *(chunk1End - 1)), 3);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, TypeChangeAfterLiteral) {
    auto elemInt32 = createElementInt32(0);
    auto elemInt64 = createElementInt64(0);

    cb.append(elemInt32);
    cb.append(elemInt64);

    BufBuilder expected;
    appendLiteral(expected, elemInt32);
    appendLiteral(expected, elemInt64);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemInt32, elemInt64});
}

TEST_F(BSONColumnTest, TypeChangeAfterSimple8b) {
    auto elemInt32 = createElementInt32(0);
    auto elemInt64 = createElementInt64(0);

    cb.append(elemInt32);
    cb.append(elemInt32);
    cb.append(elemInt64);

    BufBuilder expected;
    appendLiteral(expected, elemInt32);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, 0);
    appendLiteral(expected, elemInt64);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemInt32, elemInt32, elemInt64});
}

TEST_F(BSONColumnTest, Simple8bAfterTypeChange) {
    auto elemInt32 = createElementInt32(0);
    auto elemInt64 = createElementInt64(0);

    cb.append(elemInt32);
    cb.append(elemInt64);
    cb.append(elemInt64);

    BufBuilder expected;
    appendLiteral(expected, elemInt32);
    appendLiteral(expected, elemInt64);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, 0);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemInt32, elemInt64, elemInt64});
}

TEST_F(BSONColumnTest, BasicDouble) {
    auto d1 = createElementDouble(1.0);
    auto d2 = createElementDouble(2.0);
    cb.append(d1);
    cb.append(d2);

    BufBuilder expected;
    appendLiteral(expected, d1);
    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bBlock64(expected, deltaDouble(d2, d1, 1));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {d1, d2});
}

TEST_F(BSONColumnTest, DoubleIdenticalDeltas) {
    // This test is using identical deltas for the double type. During binary reopen it will lead to
    // a belief we can pack all these into an RLE block due to how overflow detection works (no
    // overflow in this case). However, as the value is non-zero a simple8b block will be flushed
    // when appending values and the end of the reopen process while leaving one value in pending.
    // We make sure that special double state such as the last double value in previous block is
    // stored and calculated correctly.
    std::vector<BSONElement> elems = {createElementDouble(0.0),
                                      createElementDouble(40.0),
                                      createElementDouble(80.0),
                                      createElementDouble(120.0),
                                      createElementDouble(160.0),
                                      createElementDouble(200.0),
                                      createElementDouble(240.0),
                                      createElementDouble(280.0),
                                      createElementDouble(320.0),
                                      createElementDouble(360.0)};

    for (auto&& elem : elems) {
        cb.append(elem);
    }


    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1001, 0b0001);
    appendSimple8bBlocks64(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 1.0), 2);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleOverflowEndsWithSkip) {
    // Simple8b block that cause overflow when running the binary reopen constructor ends with a
    // skip
    std::vector<BSONElement> elems = {createElementDouble(41.0),
                                      BSONElement(),
                                      BSONElement(),
                                      createElementDouble(77.0),
                                      createElementDouble(51.0),
                                      createElementDouble(53.0),
                                      BSONElement(),
                                      createElementDouble(83.0),
                                      BSONElement(),
                                      BSONElement()};

    for (auto&& elem : elems) {
        cb.append(elem);
    }


    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1001, 0b0001);
    appendSimple8bBlocks64(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 1.0), 2);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleSameScale) {
    std::vector<BSONElement> elems;
    elems.push_back(createElementDouble(1.0));
    elems.push_back(createElementDouble(2.0));
    elems.push_back(createElementDouble(3.0));

    for (const auto& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bBlocks64(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 1), 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleIncreaseScaleFromLiteral) {
    auto d1 = createElementDouble(1.0);
    auto d2 = createElementDouble(1.1);
    cb.append(d1);
    cb.append(d2);

    BufBuilder expected;
    appendLiteral(expected, d1);
    appendSimple8bControl(expected, 0b1010, 0b0000);
    appendSimple8bBlock64(expected, deltaDouble(d2, d1, 10));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {d1, d2});
}

TEST_F(BSONColumnTest, DoubleLiteralAndScaleAfterSkip) {
    auto d1 = createElementDouble(1.0);
    auto d2 = createElementDouble(1.1);
    cb.skip();
    cb.append(d1);
    cb.append(d2);

    BufBuilder expected;
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, boost::none);
    appendLiteral(expected, d1);
    appendSimple8bControl(expected, 0b1010, 0b0000);
    appendSimple8bBlock64(expected, deltaDouble(d2, d1, 10));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {BSONElement(), d1, d2});
}

TEST_F(BSONColumnTest, DoubleIncreaseScaleFromLiteralAfterSkip) {
    auto d1 = createElementDouble(1);
    auto d2 = createElementDouble(1.1);
    cb.append(d1);
    cb.skip();
    cb.skip();
    cb.append(d2);

    BufBuilder expected;
    appendLiteral(expected, d1);
    appendSimple8bControl(expected, 0b1010, 0b0000);

    std::vector<boost::optional<uint64_t>> expectedVals(2, boost::none);
    expectedVals.push_back(deltaDouble(d2, d1, 10));
    appendSimple8bBlocks64(expected, expectedVals, 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {d1, BSONElement(), BSONElement(), d2});
}

TEST_F(BSONColumnTest, DoubleIncreaseScaleFromDeltaWithRescale) {
    std::vector<BSONElement> elems;
    elems.push_back(createElementDouble(1.0));
    elems.push_back(createElementDouble(2.0));
    elems.push_back(createElementDouble(2.1));
    elems.push_back(createElementDouble(2.2));

    for (const auto& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1010, 0b0000);
    appendSimple8bBlocks64(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 10), 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleIncreaseScaleFromDeltaNoRescale) {
    std::vector<BSONElement> elems;
    elems.push_back(createElementDouble(1.1));
    elems.push_back(createElementDouble(2.1));
    elems.push_back(createElementDouble(2.2));
    elems.push_back(createElementDouble(2.3));
    elems.push_back(createElementDouble(3.12345678));

    for (const auto& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());

    auto deltaBegin = elems.begin() + 1;
    auto deltaEnd = deltaBegin + 3;
    appendSimple8bControl(expected, 0b1010, 0b0000);
    appendSimple8bBlocks64(expected, deltaDouble(deltaBegin, deltaEnd, elems.front(), 10), 1);

    appendSimple8bControl(expected, 0b1101, 0b0000);
    appendSimple8bBlocks64(
        expected, deltaDouble(deltaEnd, elems.end(), *(deltaEnd - 1), 100000000), 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleIncreaseScaleNotPossible) {
    std::vector<BSONElement> elems = {createElementDouble(-153764908544737.4),
                                      createElementDouble(-85827904635132.83)};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaDoubleMemory(elems[1], elems[0]));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleRescaleFirstRescaledIsSkip) {
    // This test writes a simple8b of doubles where one skip is left in pending. The next value need
    // a different scale factor and the test verifies we can handle this when the first rescaled
    // value is a skip.
    std::vector<BSONElement> elems = {createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      BSONElement(),
                                      createElementDouble(-0.0001),
                                      BSONElement(),
                                      createElementDouble(-0.0001),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      createElementDouble(0.0),
                                      BSONElement(),
                                      createElementDouble(std::numeric_limits<double>::infinity()),
                                      createElementDouble(std::numeric_limits<double>::infinity())};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1100, 0b0000);
    appendSimple8bBlocks64(
        expected, deltaDouble(elems.begin() + 1, elems.begin() + 31, elems.front(), 10000.0), 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected, deltaDoubleMemory(elems.begin() + 31, elems.end(), elems[30]), 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleScaleDownWithMultipleBlocksPending) {
    // This test writes a simple8b using a high scale factor followed by a large number of skips
    // that do not fit in a single simple8b block in the control with a different scale factor that
    // follows.
    std::vector<BSONElement> elems = {createElementDouble(-153764908544737.4),
                                      createElementDouble(-85827904635132.83)};
    for (int i = 0; i < 150; ++i) {
        elems.push_back(BSONElement());
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected, deltaDoubleMemory(elems.begin() + 1, elems.begin() + 2, elems.front()), 1);
    appendSimple8bControl(expected, 0b1011, 0b0010);
    appendSimple8bBlocks64(expected, deltaDouble(elems.begin() + 2, elems.end(), elems[1], 100), 3);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleScaleDownWithRLEPending) {
    // This test writes a simple8b using a high scale factor followed by a large number of skips
    // that do not fit in a single simple8b block. The builder will scale down to fit these skips as
    // the high scale factor is not needed. The number of skips in the scaled down block is an RLE
    // multiple resulting in a single RLE block in the scaled down block.
    std::vector<BSONElement> elems = {
        createElementDouble(std::numeric_limits<double>::denorm_min()),
        BSONElement(),
        createElementDouble(0.0)};
    for (int i = 0; i < 148; ++i) {
        elems.push_back(BSONElement());
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected, deltaDoubleMemory(elems.begin() + 1, elems.begin() + 31, elems.front()), 1);
    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bRLE(expected, 120);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleNoScaleDownAtSkipWithNonZeroRLEPending) {
    // This test is verifying behavior of skip for the double type when we have pending non-zero
    // RLE. Doubles are special that the last value in the previous block is tracked and the active
    // delta being tracked by the compressor can change when it decides to re-scale doubles to
    // optimally fit. After skip the latest compressor is allowed to scale down as long as there are
    // no skips in pending values as that require us to maintain the existing state. This can happen
    // when the skip flushed out pending non-zero RLE that does not evenly fit in Simple8b blocks.
    static constexpr double kBase = 32.9375;
    static constexpr double kDelta = 0.09375;
    std::vector<BSONElement> elems = {
        // uncompressed
        createElementDouble(32.8125),
        // first simple8b to allow for non-zero RLE next
        createElementDouble(kBase),
        createElementDouble(kBase + kDelta),
        // pending non-zero RLE using monotonically increasing value
        createElementDouble(kBase + kDelta * 2),
        createElementDouble(kBase + kDelta * 3),
        // this value will be part of pending RLE but will not fit in the Simple8b when the other
        // two values are written
        createElementDouble(kBase + kDelta * 4),
        BSONElement(),
        // additional value after skip, test that we have properly calculated state up to this point
        createElementDouble(33.34375)};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    // Every simple8b block written will contain a value that requires this scale factor, we can
    // therefore share a single control block for all simple8b blocks.
    appendSimple8bControl(expected, 0b1101, 0b0011);
    appendSimple8bBlocks64(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 100000000.0), 4);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleNoScaleDownAtSkipWithNonZeroRLEPendingWithLessPrecisionLast) {
    // This test is verifying behavior of skip for the double type when we have pending non-zero
    // RLE, like 'DoubleNoScaleDownAtSkipWithNonZeroRLEPending' above. The difference here is
    // that the last value written to simple8b when writing out the pending RLE has fewer decimal
    // points and could have used a smaller scale factor if written in its own block. We're testing
    // here that we don't attempt to use this scale factor for the pending values left in the
    // simple8b builder as they require larger scaling.

    static constexpr double kBase = 42.625;
    static constexpr double kDelta = 0.03125;
    std::vector<BSONElement> elems = {
        // uncompressed
        createElementDouble(kBase),
        // first simple8b to allow for non-zero RLE next
        createElementDouble(kBase + kDelta),
        createElementDouble(kBase + kDelta * 2),
        // pending non-zero RLE using monotonically increasing value
        createElementDouble(kBase + kDelta * 3),
        // last value to be written in Simple8b when encountering the skip, this has less precision
        // than the other values. We're testing that we're not attempting to use this scale factor
        // for the next block.
        createElementDouble(kBase + kDelta * 4),
        // this value will be part of pending RLE but will not fit in the Simple8b when the other
        // two values are written
        createElementDouble(kBase + kDelta * 5),
        BSONElement(),
        // additional value after skip, test that we have properly calculated state up to this point
        createElementDouble(42.78125)};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    // Every simple8b block written will contain a value that requires this scale factor, we can
    // therefore share a single control block for all simple8b blocks.
    appendSimple8bControl(expected, 0b1101, 0b0011);
    appendSimple8bBlocks64(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 100000000.0), 4);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleIncreaseScaleWithoutOverflow) {
    // This test needs to rescale doubles but can do so where there's no overflow in the new control
    std::vector<BSONElement> elems = {createElementDouble(314159264193.46228),
                                      createElementDouble(314159265898.77252),
                                      createElementDouble(314159265702.07281),
                                      createElementDouble(314159264022.27118),
                                      createElementDouble(314159264047.43854)};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1100, 0b0000);
    appendSimple8bBlocks64(
        expected, deltaDouble(elems.begin() + 1, elems.begin() + 3, elems.front(), 10000), 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {deltaDoubleMemory(elems[3], elems[2]), deltaDoubleMemory(elems[4], elems[3])},
        1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleDecreaseScaleAfterBlock) {
    std::vector<BSONElement> elems;
    elems.push_back(createElementDouble(1.12345671));
    elems.push_back(createElementDouble(1.12345672));
    elems.push_back(createElementDouble(2));
    elems.push_back(createElementDouble(3));

    for (const auto& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());

    auto deltaBegin = elems.begin() + 1;
    auto deltaEnd = deltaBegin + 2;
    appendSimple8bControl(expected, 0b1101, 0b0000);
    appendSimple8bBlocks64(
        expected, deltaDouble(deltaBegin, deltaEnd, elems.front(), 100000000), 1);

    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bBlocks64(expected, deltaDouble(deltaEnd, elems.end(), *(deltaEnd - 1), 1), 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleDecreaseScaleAfterBlockContinueAppend) {
    // The values below should result in two Simple8b blocks, one scaled with 10.0 and the second
    // scaled to 1.0. This tests that we can scale down doubles after writing a Simple8b block and
    // that we are in a good state to continue to append values.
    //
    // When the value '105.0' is appended the first Simple8b block with scale factor 10.0 will be
    // written and it will be determined that we can scale down to scale factor 1.0 for the next
    // block as '119.0' (last value previous Simple8b block) and '105.0' can both be encoded using
    // scale factor '1.0'. We then test that we can continue to append a value ('120.0') using this
    // lower scale factor.
    std::vector<BSONElement> elems = {createElementDouble(94.8),
                                      createElementDouble(107.9),
                                      createElementDouble(111.9),
                                      createElementDouble(113.4),
                                      createElementDouble(89.0),
                                      createElementDouble(126.7),
                                      createElementDouble(119.0),
                                      createElementDouble(105.0),
                                      createElementDouble(120.0)};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1010, 0b0000);
    appendSimple8bBlocks64(
        expected, deltaDouble(elems.begin() + 1, elems.begin() + 7, elems.front(), 10.0), 1);
    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bBlocks64(expected, deltaDouble(elems.begin() + 7, elems.end(), elems[6], 1.0), 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleDecreaseScaleAfterBlockUsingSkip) {
    std::vector<BSONElement> elems;
    elems.push_back(createElementDouble(1.12345671));
    elems.push_back(createElementDouble(2));
    elems.push_back(BSONElement());
    elems.push_back(BSONElement());
    elems.push_back(createElementDouble(3));

    for (const auto& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());

    auto deltaBegin = elems.begin() + 1;
    auto deltaEnd = deltaBegin + 2;
    appendSimple8bControl(expected, 0b1101, 0b0000);
    appendSimple8bBlocks64(
        expected, deltaDouble(deltaBegin, deltaEnd, elems.front(), 100000000), 1);

    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bBlocks64(expected, deltaDouble(deltaEnd, elems.end(), *deltaBegin, 1), 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleDecreaseScaleAfterBlockThenScaleBackUp) {
    std::vector<BSONElement> elems;
    elems.push_back(createElementDouble(1.12345671));
    elems.push_back(createElementDouble(1.12345672));
    elems.push_back(createElementDouble(2));
    elems.push_back(createElementDouble(3));
    elems.push_back(createElementDouble(1.12345672));

    for (const auto& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1101, 0b0001);
    appendSimple8bBlocks64(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 100000000), 2);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleDecreaseScaleAfterBlockUsingSkipThenScaleBackUp) {
    std::vector<BSONElement> elems;
    elems.push_back(createElementDouble(1.12345671));
    elems.push_back(createElementDouble(2));
    elems.push_back(BSONElement());
    elems.push_back(BSONElement());
    elems.push_back(createElementDouble(1.12345671));

    for (const auto& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1101, 0b0001);
    appendSimple8bBlocks64(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 100000000), 2);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleRescalingPreserveRLE) {
    // This test is using large deltas for the double type where the values are mostly unscalable
    // except for one of the doubles that is scalable. This will trigger the rescaling logic and
    // we're testing that the RLE state is properly preserved through the rescaling as the deltas
    // are all identical.
    static constexpr uint64_t kLargeDelta = 0x0474747474747474;

    std::vector<BSONElement> elems;
    uint64_t value = 0xbea6a6a6a6e78efc;
    elems.push_back(createElementDouble(std::bit_cast<double>(value)));
    elems.push_back(BSONElement());
    elems.push_back(createElementDouble(std::bit_cast<double>(value += kLargeDelta)));
    elems.push_back(createElementDouble(std::bit_cast<double>(value += kLargeDelta)));
    elems.push_back(createElementDouble(std::bit_cast<double>(value += kLargeDelta)));

    // One of the values are scalable, this will trigger rescaling.
    ASSERT_TRUE(Simple8bTypeUtil::encodeDouble(elems.at(2).Double(), 0).has_value());

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0011);
    appendSimple8bBlocks64(
        expected, deltaDoubleMemory(elems.begin() + 1, elems.end(), elems.front()), 4);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleUnscalable) {
    std::vector<BSONElement> elems;
    elems.push_back(createElementDouble(1.0));
    elems.push_back(createElementDouble(1.0 + std::numeric_limits<double>::epsilon()));
    elems.push_back(createElementDouble(1.0 + std::numeric_limits<double>::epsilon() * 2));

    for (const auto& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());

    std::vector<boost::optional<uint64_t>> expectedVals;
    expectedVals.push_back(deltaDoubleMemory(elems[1], elems[0]));
    expectedVals.push_back(deltaDoubleMemory(elems[2], elems[1]));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, expectedVals, 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleMultiplePendingAfterWritingBlock) {
    // This tests that we properly set '_lastValueInPrevBlock' after writing out a full Simple8b
    // block. In this test the last value in the first block will be '99.0' but the block will not
    // be written until '89.0' is appended. That means that 'previous' will be '123.0' which is not
    // the last value in previous block.
    std::vector<BSONElement> elems = {createElementDouble(116.0),
                                      createElementDouble(95.0),
                                      createElementDouble(80.0),
                                      createElementDouble(87.0),
                                      createElementDouble(113.0),
                                      createElementDouble(90.0),
                                      createElementDouble(113.0),
                                      createElementDouble(93.0),
                                      createElementDouble(99.0),
                                      createElementDouble(123.0),
                                      createElementDouble(89.0),
                                      createElementDouble(92.0)};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1001, 0b0001);
    appendSimple8bBlocks64(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 1.0), 2);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DoubleSignalingNaN) {
    auto elem = createElementDouble(0.0);
    auto nan = createElementDouble(std::numeric_limits<double>::signaling_NaN());

    cb.append(elem);
    cb.append(nan);

    BufBuilder expected;
    appendLiteral(expected, elem);

    if (auto delta = deltaDoubleMemory(nan, elem); simple8bPossible(delta)) {
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlock64(expected, delta);
    } else {
        appendLiteral(expected, nan);
    }

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elem, nan});
}

TEST_F(BSONColumnTest, DoubleQuietNaN) {
    auto elem = createElementDouble(0.0);
    auto nan = createElementDouble(std::numeric_limits<double>::quiet_NaN());

    cb.append(elem);
    cb.append(nan);

    BufBuilder expected;
    appendLiteral(expected, elem);
    if (auto delta = deltaDoubleMemory(nan, elem); simple8bPossible(delta)) {
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlock64(expected, delta);
    } else {
        appendLiteral(expected, nan);
    }
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elem, nan});
}

TEST_F(BSONColumnTest, DoubleInfinity) {
    auto elem = createElementDouble(0.0);
    auto inf = createElementDouble(std::numeric_limits<double>::infinity());

    cb.append(elem);
    cb.append(inf);

    BufBuilder expected;
    appendLiteral(expected, elem);
    if (auto delta = deltaDoubleMemory(inf, elem); simple8bPossible(delta)) {
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlock64(expected, delta);
    } else {
        appendLiteral(expected, inf);
    }
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elem, inf});
}

TEST_F(BSONColumnTest, DoubleDenorm) {
    auto elem = createElementDouble(0.0);
    auto denorm = createElementDouble(std::numeric_limits<double>::denorm_min());

    cb.append(elem);
    cb.append(denorm);

    BufBuilder expected;
    appendLiteral(expected, elem);
    if (auto delta = deltaDoubleMemory(denorm, elem); simple8bPossible(delta)) {
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlock64(expected, delta);
    } else {
        appendLiteral(expected, denorm);
    }
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elem, denorm});
}

TEST_F(BSONColumnTest, DoubleIntegerOverflow) {
    // std::numeric_limits<int64_t>::min() - 0x1000 will cause an overflow if performed as signed,
    // make sure it is handled correctly
    auto e1 = createElementDouble(
        Simple8bTypeUtil::decodeDouble(0x1000, Simple8bTypeUtil::kMemoryAsInteger));
    auto e2 = createElementDouble(Simple8bTypeUtil::decodeDouble(
        std::numeric_limits<int64_t>::min(), Simple8bTypeUtil::kMemoryAsInteger));

    cb.append(e1);
    cb.append(e2);

    BufBuilder expected;
    appendLiteral(expected, e1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaDoubleMemory(e2, e1));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {e1, e2});
}

TEST_F(BSONColumnTest, DoubleZerosSignDifference) {
    // 0.0 compares equal to -0.0 when compared as double. Make sure we can handle this case without
    // data loss.
    auto d1 = createElementDouble(0.0);
    auto d2 = createElementDouble(-0.0);
    cb.append(d1);
    cb.append(d2);

    // These numbers are encoded as a large integer that does not fit in Simple8b so the result is
    // two uncompressed literals.
    ASSERT_EQ(deltaDoubleMemory(d2, d1), 0xFFFFFFFFFFFFFFFF);

    BufBuilder expected;
    appendLiteral(expected, d1);
    appendLiteral(expected, d2);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {d1, d2});
}

TEST_F(BSONColumnTest, DoubleRle) {
    std::vector<BSONElement> elems;

    // This test uses RLE in doubles and make sure we can track lastValueInPrevBlock properly
    for (int i = 0; i < 250; ++i) {
        elems.push_back(createElementDouble(i % 124));
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1001, 0b0110);
    appendSimple8bBlocks64(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 1.0), 7);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, Decimal128Base) {
    auto elemDec128 = createElementDecimal128(Decimal128());

    cb.append(elemDec128);

    BufBuilder expected;
    appendLiteral(expected, elemDec128);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemDec128});
}

TEST_F(BSONColumnTest, Decimal128Delta) {
    auto elemDec128 = createElementDecimal128(Decimal128(1));

    cb.append(elemDec128);
    cb.append(elemDec128);

    BufBuilder expected;
    appendLiteral(expected, elemDec128);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, deltaDecimal128(elemDec128, elemDec128));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemDec128, elemDec128});
}

TEST_F(BSONColumnTest, DecimalNonZeroDelta) {
    auto elemDec128Max = createElementDecimal128(Decimal128(100));
    auto elemDec128Zero = createElementDecimal128(Decimal128(1));
    cb.append(elemDec128Zero);
    cb.append(elemDec128Max);

    BufBuilder expected;
    appendLiteral(expected, elemDec128Zero);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, deltaDecimal128(elemDec128Max, elemDec128Zero));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemDec128Zero, elemDec128Max});
}

TEST_F(BSONColumnTest, DecimalMaxMin) {
    auto elemDec128Max = createElementDecimal128(std::numeric_limits<Decimal128>::max());
    auto elemDec128Zero = createElementDecimal128(std::numeric_limits<Decimal128>::min());
    cb.append(elemDec128Zero);
    cb.append(elemDec128Max);

    BufBuilder expected;
    appendLiteral(expected, elemDec128Zero);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, deltaDecimal128(elemDec128Max, elemDec128Zero));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemDec128Zero, elemDec128Max});
}

TEST_F(BSONColumnTest, DecimalMultiElement) {
    auto elemDec128Max = createElementDecimal128(std::numeric_limits<Decimal128>::max());
    auto elemDec128Zero = createElementDecimal128(std::numeric_limits<Decimal128>::min());
    auto elemDec128One = createElementDecimal128(Decimal128(1));
    cb.append(elemDec128Zero);
    cb.append(elemDec128Max);
    cb.append(elemDec128Zero);
    cb.append(elemDec128Zero);
    cb.append(elemDec128One);

    BufBuilder expected;
    appendLiteral(expected, elemDec128Zero);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    std::vector<boost::optional<uint128_t>> valuesToAppend = {
        deltaDecimal128(elemDec128Max, elemDec128Zero),
        deltaDecimal128(elemDec128Zero, elemDec128Max),
        deltaDecimal128(elemDec128Zero, elemDec128Zero),
        deltaDecimal128(elemDec128One, elemDec128Zero)};
    appendSimple8bBlocks128(expected, valuesToAppend, 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(
        binData, {elemDec128Zero, elemDec128Max, elemDec128Zero, elemDec128Zero, elemDec128One});
}

TEST_F(BSONColumnTest, DecimalMultiElementSkips) {
    auto elemDec128Max = createElementDecimal128(std::numeric_limits<Decimal128>::max());
    auto elemDec128Zero = createElementDecimal128(std::numeric_limits<Decimal128>::min());
    auto elemDec128One = createElementDecimal128(Decimal128(1));
    cb.append(elemDec128Zero);
    cb.append(elemDec128Max);
    cb.skip();
    cb.skip();
    cb.append(elemDec128Zero);
    cb.append(elemDec128Zero);
    cb.append(elemDec128One);

    BufBuilder expected;
    appendLiteral(expected, elemDec128Zero);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    std::vector<boost::optional<uint128_t>> valuesToAppend = {
        deltaDecimal128(elemDec128Max, elemDec128Zero),
        boost::none,
        boost::none,
        deltaDecimal128(elemDec128Zero, elemDec128Max),
        deltaDecimal128(elemDec128Zero, elemDec128Zero),
        deltaDecimal128(elemDec128One, elemDec128Zero)};
    appendSimple8bBlocks128(expected, valuesToAppend, 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData,
                        {elemDec128Zero,
                         elemDec128Max,
                         BSONElement(),
                         BSONElement(),
                         elemDec128Zero,
                         elemDec128Zero,
                         elemDec128One});
}

TEST_F(BSONColumnTest, BasicObjectId) {
    auto first = createObjectId(OID("112233445566778899AABBCC"));
    // Increment the lower byte for timestamp and counter.
    auto second = createObjectId(OID("112233455566778899AABBEE"));
    // Increment the lower byte for counter.
    auto third = createObjectId(OID("112233455566778899AABBFF"));

    cb.append(first);
    cb.append(second);
    cb.append(second);
    cb.append(third);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    std::vector<boost::optional<uint64_t>> expectedDeltas{
        deltaOfDeltaObjectId(second, first, first),
        deltaOfDeltaObjectId(second, second, first),
        deltaOfDeltaObjectId(third, second, second)};
    appendSimple8bBlocks64(expected, expectedDeltas, 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second, second, third});
}

TEST_F(BSONColumnTest, ObjectIdDifferentProcessUnique) {
    auto first = createObjectId(OID("112233445566778899AABBCC"));
    auto second = createObjectId(OID("112233445566FF8899AABBCC"));

    cb.append(first);
    cb.append(second);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendLiteral(expected, second);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second});
}

TEST_F(BSONColumnTest, ObjectIdAfterChangeBack) {
    auto first = createObjectId(OID("112233445566778899AABBCC"));
    // Increment the lower byte for timestamp and counter.
    auto second = createObjectId(OID("1122FF445566778899AABBEE"));
    auto elemInt32 = createElementInt32(0);

    cb.append(first);
    cb.append(second);
    cb.append(elemInt32);
    cb.append(first);
    cb.append(second);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaOfDeltaObjectId(second, first, first));

    appendLiteral(expected, elemInt32);

    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaOfDeltaObjectId(second, first, first));

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second, elemInt32, first, second});
}

TEST_F(BSONColumnTest, Simple8bTimestamp) {
    auto first = createTimestamp(Timestamp(0));
    auto second = createTimestamp(Timestamp(1));

    cb.append(first);
    cb.append(second);
    cb.append(second);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    std::vector<boost::optional<uint64_t>> expectedDeltaOfDeltas{
        deltaOfDeltaTimestamp(second, first), deltaOfDeltaTimestamp(second, second, first)};
    appendSimple8bBlocks64(expected, expectedDeltaOfDeltas, 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second, second});
}

TEST_F(BSONColumnTest, Simple8bTimestampNegativeDeltaOfDelta) {
    auto first = createTimestamp(Timestamp(3));
    auto second = createTimestamp(Timestamp(5));
    auto third = createTimestamp(Timestamp(6));

    cb.append(first);
    cb.append(second);
    cb.append(third);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    std::vector<boost::optional<uint64_t>> expectedDeltaOfDeltas{
        deltaOfDeltaTimestamp(second, first), deltaOfDeltaTimestamp(third, second, first)};
    appendSimple8bBlocks64(expected, expectedDeltaOfDeltas, 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second, third});
}

TEST_F(BSONColumnTest, Simple8bTimestampAfterChangeBack) {
    auto first = createTimestamp(Timestamp(0));
    auto second = createTimestamp(Timestamp(1));
    auto elemInt32 = createElementInt32(0);

    cb.append(first);
    cb.append(second);
    cb.append(elemInt32);
    cb.append(first);  // Test confirms that _prevTimestampDelta gets reset to 0.
    cb.append(second);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaOfDeltaTimestamp(second, first));

    appendLiteral(expected, elemInt32);

    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaOfDeltaTimestamp(second, first));

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second, elemInt32, first, second});
}

TEST_F(BSONColumnTest, LargeDeltaOfDeltaTimestamp) {
    auto first = createTimestamp(Timestamp(0));
    cb.append(first);

    auto second = createTimestamp(Timestamp(std::numeric_limits<int64_t>::max()));
    cb.append(second);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendLiteral(expected, second);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second});
}

TEST_F(BSONColumnTest, LargeDeltaOfDeltaIsLiteralAfterSimple8bTimestamp) {
    auto zero = createTimestamp(Timestamp(0));
    cb.append(zero);
    cb.append(zero);

    auto large = createTimestamp(Timestamp(std::numeric_limits<int64_t>::max()));
    cb.append(large);
    cb.append(large);

    // Semi-large number so that the delta-of-delta will fit into a Simple8b word.
    auto semiLarge = createTimestamp(Timestamp(0x7FFFFFFFFF000000));
    cb.append(semiLarge);

    BufBuilder expected;
    appendLiteral(expected, zero);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaOfDeltaTimestamp(zero, zero));
    appendLiteral(expected, large);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    std::vector<boost::optional<uint64_t>> expectedDeltaOfDeltas{
        deltaOfDeltaTimestamp(large, large), deltaOfDeltaTimestamp(semiLarge, large, large)};
    appendSimple8bBlocks64(expected, expectedDeltaOfDeltas, 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {zero, zero, large, large, semiLarge});
}

TEST_F(BSONColumnTest, DateBasic) {
    auto first = createDate(Date_t::fromMillisSinceEpoch(1));
    auto second = createDate(Date_t::fromMillisSinceEpoch(2));
    cb.append(first);
    cb.append(second);
    cb.append(second);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    std::vector<boost::optional<uint64_t>> expectedDeltaOfDeltas{
        deltaOfDeltaDate(second, first, first), deltaOfDeltaDate(second, second, first)};
    _appendSimple8bBlocks(expected, expectedDeltaOfDeltas, 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second, second});
}

TEST_F(BSONColumnTest, DateAfterChangeBack) {
    auto date = createDate(Date_t::fromMillisSinceEpoch(1));
    auto elemInt32 = createElementInt32(0);

    cb.append(elemInt32);
    cb.append(date);
    cb.append(date);

    BufBuilder expected;
    appendLiteral(expected, elemInt32);
    appendLiteral(expected, date);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemInt32, date, date});
}

TEST_F(BSONColumnTest, DateLargeDelta) {
    auto first = createDate(Date_t::fromMillisSinceEpoch(1));
    cb.append(first);

    auto second = createDate(Date_t::fromMillisSinceEpoch(std::numeric_limits<int64_t>::max()));
    cb.append(second);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendLiteral(expected, second);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second});
}

TEST_F(BSONColumnTest, BoolBasic) {
    auto trueBson = createBool(true);
    auto falseBson = createBool(false);
    cb.append(trueBson);
    cb.append(trueBson);
    cb.append(falseBson);
    cb.append(trueBson);

    BufBuilder expected;
    appendLiteral(expected, trueBson);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    std::vector<boost::optional<uint64_t>> expectedDeltaOfDeltas{deltaBool(trueBson, trueBson),
                                                                 deltaBool(falseBson, trueBson),
                                                                 deltaBool(trueBson, falseBson)};
    _appendSimple8bBlocks(expected, expectedDeltaOfDeltas, 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {trueBson, trueBson, falseBson, trueBson});
}

TEST_F(BSONColumnTest, BoolAfterChangeBack) {
    auto trueBson = createBool(true);
    auto elemInt32 = createElementInt32(0);

    cb.append(elemInt32);
    cb.append(trueBson);
    cb.append(trueBson);

    BufBuilder expected;
    appendLiteral(expected, elemInt32);
    appendLiteral(expected, trueBson);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemInt32, trueBson, trueBson});
}

TEST_F(BSONColumnTest, UndefinedBasic) {
    auto first = createUndefined();
    cb.append(first);
    cb.append(first);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, first});
}

TEST_F(BSONColumnTest, UndefinedAfterChangeBack) {
    auto undefined = createUndefined();
    auto elemInt32 = createElementInt32(0);

    cb.append(elemInt32);
    cb.append(undefined);
    cb.append(undefined);

    BufBuilder expected;
    appendLiteral(expected, elemInt32);
    appendLiteral(expected, undefined);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemInt32, undefined, undefined});
}

TEST_F(BSONColumnTest, NullBasic) {
    auto first = createNull();
    cb.append(first);
    cb.append(first);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, first});
}

TEST_F(BSONColumnTest, NullAfterChangeBack) {
    auto null = createNull();
    auto elemInt32 = createElementInt32(0);

    cb.append(elemInt32);
    cb.append(null);
    cb.append(null);

    BufBuilder expected;
    appendLiteral(expected, elemInt32);
    appendLiteral(expected, null);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemInt32, null, null});
}

TEST_F(BSONColumnTest, RegexBasic) {
    auto first = createRegex();
    auto second = createRegex("regex");
    cb.append(first);
    cb.append(second);
    cb.append(second);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendLiteral(expected, second);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second, second});
}

TEST_F(BSONColumnTest, RegexBasicWithOptions) {
    auto first = createRegex("regex", "ims");
    auto second = createRegex("regex");
    cb.append(first);
    cb.append(first);
    cb.append(second);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendLiteral(expected, second);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, first, second});
}

TEST_F(BSONColumnTest, RegexAfterChangeBack) {
    auto regex = createRegex();
    auto elemInt32 = createElementInt32(0);

    cb.append(elemInt32);
    cb.append(regex);
    cb.append(regex);

    BufBuilder expected;
    appendLiteral(expected, elemInt32);
    appendLiteral(expected, regex);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemInt32, regex, regex});
}

TEST_F(BSONColumnTest, RegexAfterChangeBackWithOption) {
    auto regexWithOptions = createRegex("regex", "xu");
    auto elemInt32 = createElementInt32(0);

    cb.append(regexWithOptions);
    cb.append(regexWithOptions);
    cb.append(elemInt32);

    BufBuilder expected;
    appendLiteral(expected, regexWithOptions);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendLiteral(expected, elemInt32);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {regexWithOptions, regexWithOptions, elemInt32});
}

TEST_F(BSONColumnTest, DBRefBasic) {
    auto oid = OID("112233445566778899AABBCC");
    auto first = createDBRef("ns", oid);
    auto second = createDBRef("diffNs", oid);
    cb.append(first);
    cb.append(second);
    cb.append(second);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendLiteral(expected, second);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second, second});
}

TEST_F(BSONColumnTest, DBRefAfterChangeBack) {
    auto oid = OID("112233445566778899AABBCC");
    auto dbRef = createDBRef("ns", oid);
    auto elemInt32 = createElementInt32(0);

    cb.append(elemInt32);
    cb.append(dbRef);
    cb.append(dbRef);

    BufBuilder expected;
    appendLiteral(expected, elemInt32);
    appendLiteral(expected, dbRef);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemInt32, dbRef, dbRef});
}

TEST_F(BSONColumnTest, CodeWScopeBasic) {
    auto first = createCodeWScope("code", BSONObj());
    auto second = createCodeWScope("diffCode", BSONObj());
    cb.append(first);
    cb.append(second);
    cb.append(second);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendLiteral(expected, second);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second, second});
}

TEST_F(BSONColumnTest, CodeWScopeAfterChangeBack) {
    auto codeWScope = createCodeWScope("code", BSONObj());
    auto elemInt32 = createElementInt32(0);

    cb.append(elemInt32);
    cb.append(codeWScope);
    cb.append(codeWScope);

    BufBuilder expected;
    appendLiteral(expected, elemInt32);
    appendLiteral(expected, codeWScope);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemInt32, codeWScope, codeWScope});
}

TEST_F(BSONColumnTest, SymbolBasic) {
    auto first = createSymbol("symbol");
    auto second = createSymbol("diffSymbol");
    cb.append(first);
    cb.append(second);
    cb.append(second);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendLiteral(expected, second);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {first, second, second});
}

TEST_F(BSONColumnTest, SymbolAfterChangeBack) {
    auto symbol = createSymbol("symbol");
    auto elemInt32 = createElementInt32(0);

    cb.append(elemInt32);
    cb.append(symbol);
    cb.append(symbol);

    BufBuilder expected;
    appendLiteral(expected, elemInt32);
    appendLiteral(expected, symbol);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemInt32, symbol, symbol});
}

TEST_F(BSONColumnTest, BinDataBase) {
    std::vector<uint8_t> input{'1', '2', '3', '4'};
    auto elemBinData = createElementBinData(BinDataGeneral, input);

    cb.append(elemBinData);

    BufBuilder expected;
    appendLiteral(expected, elemBinData);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemBinData});
}

TEST_F(BSONColumnTest, BinDataOdd) {
    std::vector<uint8_t> input{'\n', '2', '\n', '4'};
    auto elemBinData = createElementBinData(BinDataGeneral, input);

    cb.append(elemBinData);

    BufBuilder expected;
    appendLiteral(expected, elemBinData);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemBinData});
}

TEST_F(BSONColumnTest, BinDataDelta) {
    std::vector<uint8_t> input{'1', '2', '3', '4'};
    auto elemBinData = createElementBinData(BinDataGeneral, input);

    cb.append(elemBinData);
    cb.append(elemBinData);

    BufBuilder expected;
    appendLiteral(expected, elemBinData);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, deltaBinData(elemBinData, elemBinData));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemBinData, elemBinData});
}

TEST_F(BSONColumnTest, BinDataDeltaCountDifferenceShouldFail) {
    std::vector<uint8_t> input{'1', '2', '3', '4'};
    auto elemBinData = createElementBinData(BinDataGeneral, input);

    cb.append(elemBinData);

    std::vector<uint8_t> inputLong{'1', '2', '3', '4', '5'};
    auto elemBinDataLong = createElementBinData(BinDataGeneral, inputLong);
    cb.append(elemBinDataLong);

    BufBuilder expected;
    appendLiteral(expected, elemBinData);
    appendLiteral(expected, elemBinDataLong);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemBinData, elemBinDataLong});
}

TEST_F(BSONColumnTest, BinDataDeltaTypeDifferenceShouldFail) {
    std::vector<uint8_t> input{'1', '2', '3', '4'};
    auto elemBinData = createElementBinData(BinDataGeneral, input);

    cb.append(elemBinData);

    auto elemBinDataDifferentType = createElementBinData(Function, input);
    cb.append(elemBinDataDifferentType);

    BufBuilder expected;
    appendLiteral(expected, elemBinData);
    appendLiteral(expected, elemBinDataDifferentType);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemBinData, elemBinDataDifferentType});
}

TEST_F(BSONColumnTest, BinDataDeltaCheckSkips) {
    std::vector<uint8_t> input{'1', '2', '3', '4'};
    auto elemBinData = createElementBinData(BinDataGeneral, input);

    cb.append(elemBinData);

    std::vector<uint8_t> inputLong{'1', '2', '3', '3'};
    auto elemBinDataLong = createElementBinData(BinDataGeneral, inputLong);
    cb.append(elemBinDataLong);
    cb.skip();
    cb.append(elemBinData);

    BufBuilder expected;
    appendLiteral(expected, elemBinData);
    appendSimple8bControl(expected, 0b1000, 0b0001);
    std::vector<boost::optional<uint128_t>> expectedValues = {
        deltaBinData(elemBinDataLong, elemBinData),
        boost::none,
        deltaBinData(elemBinData, elemBinDataLong)};
    appendSimple8bBlocks128(expected, expectedValues, 2);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemBinData, elemBinDataLong, BSONElement(), elemBinData});
}

TEST_F(BSONColumnTest, BinDataLargerThan16) {
    std::vector<uint8_t> input{
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '1', '2', '3', '4', '5', '6', '7', '8'};
    auto elemBinData = createElementBinData(BinDataGeneral, input);

    cb.append(elemBinData);

    std::vector<uint8_t> inputLong{
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '1', '2', '3', '4', '5', '6', '7', '9'};
    auto elemBinDataLong = createElementBinData(BinDataGeneral, inputLong);
    cb.append(elemBinDataLong);

    BufBuilder expected;
    appendLiteral(expected, elemBinData);
    appendLiteral(expected, elemBinDataLong);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemBinData, elemBinDataLong});
}

TEST_F(BSONColumnTest, BinDataLargerThan16InterleavedDuplicatePath) {
    // This test is similar to the above but verifies that we keep track of decompression state
    // correctly even when there are duplicate paths.
    std::vector<uint8_t> bytes{
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '1', '2', '3', '4', '5', '6', '7', '8'};
    std::vector<BSONElement> elems;
    const size_t kNElems = 8;
    for (size_t i = 0; i < kNElems; ++i) {
        elems.push_back(createElementObj(
            BSON("a" << createElementBinData(BinDataGeneral, bytes) << "b" << int64_t(i))));
        cb.append(elems.back());
    }

    auto binData = cb.finalize();
    verifyDecompression(binData, elems);

    // Test that we can decompress duplicate paths that refer to the same element.
    std::vector<TestPath> testPaths{TestPath{{"a"}}, TestPath{{"a"}}};
    verifyDecompressPathFast(binData, elems, testPaths);
}

TEST_F(BSONColumnTest, BinDataEqualTo16) {
    std::vector<uint8_t> input{
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '1', '2', '3', '4', '5', '6', '7'};
    auto elemBinData = createElementBinData(BinDataGeneral, input);

    cb.append(elemBinData);

    std::vector<uint8_t> inputLong{
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '1', '2', '3', '4', '5', '6', '8'};
    auto elemBinDataLong = createElementBinData(BinDataGeneral, inputLong);
    cb.append(elemBinDataLong);

    BufBuilder expected;
    appendLiteral(expected, elemBinData);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, deltaBinData(elemBinDataLong, elemBinData));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemBinData, elemBinDataLong});
}

TEST_F(BSONColumnTest, BinDataLargerThan16SameValue) {
    std::vector<uint8_t> input{
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '1', '2', '3', '4', '5', '6', '7', '8'};
    auto elemBinData = createElementBinData(BinDataGeneral, input);

    cb.append(elemBinData);
    cb.append(elemBinData);

    BufBuilder expected;
    appendLiteral(expected, elemBinData);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, deltaBinData(elemBinData, elemBinData));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemBinData, elemBinData});
}

TEST_F(BSONColumnTest, BinDataLargerThan16SameValueWithSkip) {
    std::vector<uint8_t> input{
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '1', '2', '3', '4', '5', '6', '7', '8'};
    auto elemBinData = createElementBinData(BinDataGeneral, input);

    cb.append(elemBinData);
    cb.skip();
    cb.append(elemBinData);

    BufBuilder expected;
    appendLiteral(expected, elemBinData);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    std::vector<boost::optional<uint128_t>> expectedValues = {
        boost::none, deltaBinData(elemBinData, elemBinData)};
    appendSimple8bBlocks128(expected, expectedValues, 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemBinData, BSONElement(), elemBinData});
}

TEST_F(BSONColumnTest, StringBase) {
    auto elem = createElementString("test");
    cb.append(elem);

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elem});
}

TEST_F(BSONColumnTest, StringDeltaSame) {
    auto elemString = createElementString("test");
    cb.append(elemString);
    cb.append(elemString);

    BufBuilder expected;
    appendLiteral(expected, elemString);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, deltaString(elemString, elemString));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemString, elemString});
}

TEST_F(BSONColumnTest, StringDeltaDiff) {
    auto elemString = createElementString("mongo");
    cb.append(elemString);
    auto elemString2 = createElementString("tests");
    cb.append(elemString2);

    BufBuilder expected;
    appendLiteral(expected, elemString);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, deltaString(elemString2, elemString));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemString, elemString2});
}

TEST_F(BSONColumnTest, StringDeltaLarge) {
    auto elemString = createElementString("mongoaaaaaaa");
    cb.append(elemString);
    // Need to make sure we have a significant overlap in delta so we can have a trailingZeroCount
    // thats viable.
    auto elemString2 = createElementString("testxaaaaaaa");
    cb.append(elemString2);

    BufBuilder expected;
    appendLiteral(expected, elemString);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, deltaString(elemString2, elemString));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemString, elemString2});
}

TEST_F(BSONColumnTest, StringAfterInvalid) {
    auto elem = createElementString("mongo");
    cb.append(elem);

    auto elemInvalid = createElementString("\0mongo"_sd);
    cb.append(elemInvalid);

    auto elem2 = createElementString("test");
    cb.append(elem2);

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendLiteral(expected, elemInvalid);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(
        expected,
        Simple8bTypeUtil::encodeInt128(*Simple8bTypeUtil::encodeString(elem2.valueStringData())));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elem, elemInvalid, elem2});
}

TEST_F(BSONColumnTest, StringEmptyAfterLarge) {
    auto large = createElementString(std::string(32, 'a'));
    cb.append(large);
    auto empty = createElementString("");
    // Confirm that empty string is encoded as 0 which this test relies on.
    ASSERT_EQ(*Simple8bTypeUtil::encodeString(empty.valueStringData()), 0);
    cb.append(empty);

    BufBuilder expected;
    appendLiteral(expected, large);
    // The empty string must be stored as full literal to avoid ambiguity with repeat of previous.
    appendLiteral(expected, empty);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {large, empty});
}

TEST_F(BSONColumnTest, RepeatInvalidString) {
    auto elem = createElementString("mongo");
    cb.append(elem);

    auto elemInvalid = createElementString("\0mongo"_sd);
    cb.append(elemInvalid);
    cb.append(elemInvalid);

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendLiteral(expected, elemInvalid);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, kDeltaForBinaryEqualValues128);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elem, elemInvalid, elemInvalid});
}


TEST_F(BSONColumnTest, BinDataLargerThan16WithNonZeroDelta) {
    // This interleaved binary is invalid. It has a reference object of type BinData that is larger
    // than 16 bytes, followed 7 simple8b blocks with 7502 elements inside. The delta block contains
    // all zeroes or missing elements except for a non-zero element in the last place. This should
    // produce an error when being decompressed because we cannot apply a delta to a bindata larger
    // than 16 bytes. This specific binary produces incorrect results when decompressed with a
    // in64_t simple8b decoder, and must use a int128_t simple8b decoder. We will verify that both
    // the block-based and iterative implementation throw an error.
    StringData b64Encoded =
        "8SwAAAAFACAAAAAAf/4BCLHOzwAG/////////2l/AAsACgBbAAEAegATaX8Ahn8A//gj/wD///8h/wH+AADf+CP/AP///yH/Af4A/6mX/2Z/AH/4AH9/Mn9gZXQAgGj/////AH8AAAA="_sd;
    std::string interleavedBinary = base64::decode(b64Encoded);

    // Verify block-based interleaved decompression throws an error.
    {
        boost::intrusive_ptr allocator{new BSONElementStorage()};
        BSONColumnBlockBased colBlockBased{interleavedBinary.data(),
                                           static_cast<size_t>(interleavedBinary.size())};
        std::vector<BSONElement> collection;
        ASSERT_THROWS_CODE(colBlockBased.decompress<BSONElementMaterializer>(collection, allocator),
                           DBException,
                           8690000);
    }

    // Verify block-based path decompression throws an error.
    {
        boost::intrusive_ptr allocator{new BSONElementStorage()};
        BSONColumnBlockBased colBlockBased{interleavedBinary.data(),
                                           static_cast<size_t>(interleavedBinary.size())};
        std::vector<BSONElement> collection;
        // Get the field in the object (the field name is the empty string)
        std::vector<std::pair<TestPath, std::vector<BSONElement>&>> testPaths{
            {TestPath{{""}}, collection}};
        ASSERT_THROWS_CODE(
            colBlockBased.decompress<BSONElementMaterializer>(allocator, std::span(testPaths)),
            DBException,
            8609800);
    }

    // Build a similar BSONColumn that has the delta block not in interleaved mode.
    BufBuilder scalarBinary;
    {
        BSONObj obj{interleavedBinary.data() + 1};
        BSONElement elem = obj.firstElement();

        // Append the BinData literal.
        appendLiteral(scalarBinary, elem);
        // This is the control byte and delta block with 7502 elements.
        scalarBinary.appendBuf(interleavedBinary.data() + 45, (102 - 45));
        appendEOO(scalarBinary);
    }

    // Verify the iterative implementation throws an error.
    BSONColumn col{scalarBinary.buf(), static_cast<size_t>(scalarBinary.len())};
    ASSERT_THROWS_CODE(std::distance(col.begin(), col.end()), DBException, 8412601);

    // Verify non-interleaved block-based decompression throws an error.
    {
        boost::intrusive_ptr allocator{new BSONElementStorage()};
        BSONColumnBlockBased colBlockBased{scalarBinary.buf(),
                                           static_cast<size_t>(scalarBinary.len())};
        std::vector<BSONElement> collection;
        ASSERT_THROWS_CODE(colBlockBased.decompress<BSONElementMaterializer>(collection, allocator),
                           DBException,
                           8609800);
    }
}

TEST_F(BSONColumnTest, EmptyStringAfterUnencodable) {
    std::vector<BSONElement> elems = {createElementString("\0"_sd), createElementString(""_sd)};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems[0]);
    appendLiteral(expected, elems[1]);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, UnencodableStringWithZeroDelta) {
    std::vector<BSONElement> elems = {createElementString("\0"_sd), createElementString("\0"_sd)};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems[0]);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, uint128_t(0));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, EmptyStringAfterUnencodableDelta) {
    std::vector<BSONElement> elems = {
        createElementString("\0"_sd), createElementString("\0"_sd), createElementString(""_sd)};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems[0]);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, uint128_t(0));
    appendLiteral(expected, elems[2]);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, EmptyStringAfterUnencodableLiteralAndDelta) {
    std::vector<BSONElement> elems = {createElementString("\0"_sd),
                                      createElementString("a"_sd),
                                      createElementString(""_sd),
                                      createElementString(""_sd)};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    // As the literal is unencodable it will be considered to have a 0 encoding. We take this from
    // the last element that is empty string.
    appendSimple8bBlocks128(expected, deltaString(elems.begin() + 1, elems.end(), elems.back()), 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, UnencodableStringBetweenZeroDelta) {
    std::vector<BSONElement> elems = {
        createElementString("a"_sd),
        createElementString("\0"_sd),
        createElementString("s"_sd),
        createElementString("s"_sd),
        createElementString("\0"_sd),
        createElementString("\0"_sd),
    };

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems[0]);
    appendLiteral(expected, elems[1]);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks128(
        expected, {deltaString(elems[2], elems[1]), deltaString(elems[3], elems[2])}, 1);
    appendLiteral(expected, elems[4]);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, uint128_t(0));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, StringMultiType) {
    // Add decimals first
    auto elemDec128Max = createElementDecimal128(std::numeric_limits<Decimal128>::max());
    auto elemDec128Zero = createElementDecimal128(std::numeric_limits<Decimal128>::min());
    auto elemDec128One = createElementDecimal128(Decimal128(1));
    cb.append(elemDec128Zero);
    cb.append(elemDec128Max);
    cb.append(elemDec128Zero);
    cb.append(elemDec128Zero);
    cb.append(elemDec128One);

    // Add strings
    auto elemString = createElementString("mongoisgreat");
    cb.append(elemString);
    // Need to make sure we have a significant overlap in delta so we can have a trailingZeroCount
    // thats viable.
    auto elemString2 = createElementString("testisagreat");
    cb.append(elemString2);

    BufBuilder expected;
    appendLiteral(expected, elemDec128Zero);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    std::vector<boost::optional<uint128_t>> valuesToAppend = {
        deltaDecimal128(elemDec128Max, elemDec128Zero),
        deltaDecimal128(elemDec128Zero, elemDec128Max),
        deltaDecimal128(elemDec128Zero, elemDec128Zero),
        deltaDecimal128(elemDec128One, elemDec128Zero)};
    appendSimple8bBlocks128(expected, valuesToAppend, 1);
    appendLiteral(expected, elemString);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, deltaString(elemString2, elemString));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData,
                        {elemDec128Zero,
                         elemDec128Max,
                         elemDec128Zero,
                         elemDec128Zero,
                         elemDec128One,
                         elemString,
                         elemString2});
}

TEST_F(BSONColumnTest, Int64FullControlWithPendingAtFinalize) {
    // This test completely fills up a control byte with 16 simple8b blocks with the append calls
    // while leaving the last element in pending. Finalizing will create a new control byte for this
    // last element. For bucket reopen, we will overflow in the last control byte and but fill it
    // completely again when appending the pending values. While the overflow code is triggered the
    // end result should be identical to as-if we just looked at the current control and never
    // overflowed.
    std::vector<BSONElement> elems = {
        createElementInt64(0x3230373139),   createElementInt64(0x3234373139),
        createElementInt64(0x3236373138),   createElementInt64(0x3238393138),
        createElementInt64(0x323c393137),   createElementInt64(0x323e393137),
        createElementInt64(0x323e3b3137),   createElementInt64(0x32403b3136),
        createElementInt64(0x32443b3136),   createElementInt64(0x642e42293136),
        createElementInt64(0x643242293136), createElementInt64(0x643442293135),
        createElementInt64(0x6434422b3135), createElementInt64(0x6436422b3134),
        createElementInt64(0x643a422b3134), createElementInt64(0x643e42293133),
        createElementInt64(0x644242293133), createElementInt64(0x644442293132),
        createElementInt64(0x644642273131), createElementInt64(0x644842273131),
        createElementInt64(0x644a42273131), createElementInt64(0x644a42293131),
        createElementInt64(0x644c42293130), createElementInt64(0x644e4229312f)};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks64(
        expected,
        deltaInt64(elems.begin() + 1, elems.begin() + elems.size() - 1, elems.front()),
        16);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaInt64(elems.back(), elems.at(22)));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, StringFullControlWithPendingAtFinalize) {
    // This test completely fills up a control byte with 16 simple8b blocks with the append calls
    // while leaving the last element in pending. Finalizing will create a new control byte for this
    // last element. For bucket reopen, we will overflow in the last control byte and but fill it
    // completely again when appending the pending values. While the overflow code is triggered the
    // end result should be identical to as-if we just looked at the current control and never
    // overflowed.
    std::vector<BSONElement> elems = {
        createElementString("20719"_sd),  createElementString("22719"_sd),
        createElementString("21719"_sd),  createElementString("22819"_sd),
        createElementString("20819"_sd),  createElementString("21819"_sd),
        createElementString("21919"_sd),  createElementString("20919"_sd),
        createElementString("22919"_sd),  createElementString("201019"_sd),
        createElementString("221019"_sd), createElementString("211019"_sd),
        createElementString("211119"_sd), createElementString("201119"_sd),
        createElementString("221119"_sd), createElementString("201219"_sd),
        createElementString("221219"_sd), createElementString("211219"_sd),
        createElementString("201319"_sd), createElementString("211319"_sd),
        createElementString("221319"_sd), createElementString("221419"_sd),
        createElementString("211419"_sd), createElementString("201419"_sd)};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks128(
        expected,
        deltaString(elems.begin() + 1, elems.begin() + elems.size() - 1, elems.front()),
        16);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, deltaString(elems.back(), elems.at(22)));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, CodeBase) {
    auto elem = createElementCode("test");
    cb.append(elem);

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elem});
}

TEST_F(BSONColumnTest, CodeDeltaSame) {
    auto elemCode = createElementCode("test");
    cb.append(elemCode);
    cb.append(elemCode);

    BufBuilder expected;
    appendLiteral(expected, elemCode);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, deltaString(elemCode, elemCode));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemCode, elemCode});
}

TEST_F(BSONColumnTest, CodeDeltaDiff) {
    auto elemCode = createElementCode("mongo");
    cb.append(elemCode);
    auto elemCode2 = createElementCode("tests");
    cb.append(elemCode2);

    BufBuilder expected;
    appendLiteral(expected, elemCode);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected, deltaString(elemCode2, elemCode));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {elemCode, elemCode2});
}

TEST_F(BSONColumnTest, ObjectUncompressed) {
    // BSONColumnBuilder does not produce this kind of binary where Objects are stored uncompressed.
    // However they are valid according to the specification so verify that we can decompress.
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << 2)),
                                      createElementObj(BSON("x" << 1 << "y" << 3))};

    BufBuilder expected;
    for (auto elem : elems) {
        appendLiteral(expected, elem);
    }
    appendEOO(expected);

    BSONBinData data;
    data.data = expected.buf();
    data.length = expected.len();
    data.type = BinDataType::Column;
    verifyDecompression(data, elems);
}

TEST_F(BSONColumnTest, ObjectEqual) {
    // BSONColumnBuilder does not produce this kind of binary where Objects are stored uncompressed.
    // However they are valid according to the specification so verify that we can decompress.

    auto elemObj = createElementObj(BSON("x" << 1 << "y" << 2));
    std::vector<BSONElement> elems = {elemObj, elemObj};

    BufBuilder expected;
    appendLiteral(expected, elemObj);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    BSONBinData data;
    data.data = expected.buf();
    data.length = expected.len();
    data.type = BinDataType::Column;
    verifyDecompression(data, elems);
}

TEST_F(BSONColumnTest, ArrayUncompressed) {
    std::vector<BSONElement> elems = {createElementArray(BSON_ARRAY(1 << 2)),
                                      createElementArray(BSON_ARRAY(1 << 2 << 3))};

    BufBuilder expected;
    for (auto elem : elems) {
        appendLiteral(expected, elem);
    }
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, ArrayEqual) {
    auto elemObj = createElementArray(BSON_ARRAY("a" << "b"));
    std::vector<BSONElement> elems = {elemObj, elemObj};

    BufBuilder expected;
    appendLiteral(expected, elemObj);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, DeltasWithNoUncompressedByte) {
    // This test validates decoding any deltas before an uncompressed byte will return EOO
    // BSONElements.
    BufBuilder expected;
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {deltaInt64(createElementInt64(10), createElementInt64(1)),
                            deltaInt64(createElementInt64(20), createElementInt64(10)),
                            kDeltaForBinaryEqualValues},
                           1);
    appendEOO(expected);

    verifyDecompression(expected, {BSONElement(), BSONElement(), BSONElement()});
}

TEST_F(BSONColumnTest, OnlySkipManyTwoControlBytes) {
    // This test validates handling when we have so many consecutive skips that they span over two
    // control blocks. We need to write at least 17 simple8b blocks for this to be the case. As all
    // values are the same the RLE blocks will need the max amount of values which is 1920. When
    // appending skips, we will first get a non-RLE block containing 60 values. Then we add 59 more
    // at the end that need to be split up in 4 simple8b blocks. By using 12 full RLE blocks we then
    // get 17 simple8b blocks in total which require two bsoncolumn control bytes.
    size_t num = /*first non-RLE*/ 60 + /*RLE*/ 1920 * 12 + /*non-RLE at end*/ 59;
    for (size_t i = 0; i < num; ++i) {
        cb.skip();
    }

    BufBuilder expected;
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks64(
        expected, std::vector<boost::optional<uint64_t>>(num - 1, boost::none), 16);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, std::vector<boost::optional<uint64_t>>(1, boost::none), 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected, false);
    verifyColumnReopenFromBinary(reinterpret_cast<const char*>(binData.data), binData.length);
}

TEST_F(BSONColumnTest, BulkSkipConstructor) {
    size_t num = /*first non-RLE*/ 60 + /*RLE*/ 1920 * 12 + /*non-RLE at end*/ 59;
    BSONColumnBuilder<> skipCb(num);

    BufBuilder expected;
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks64(
        expected, std::vector<boost::optional<uint64_t>>(num - 1, boost::none), 16);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, std::vector<boost::optional<uint64_t>>(1, boost::none), 1);
    appendEOO(expected);

    auto binData = skipCb.finalize();
    verifyBinary(binData, expected, false);
    verifyColumnReopenFromBinary(reinterpret_cast<const char*>(binData.data), binData.length);
}

TEST_F(BSONColumnTest, BulkSkipConstructorValueAfterSmall) {
    auto elem = createElementInt32(1);
    BSONColumnBuilder<> skipCb(1);
    skipCb.append(elem);

    BufBuilder expected;
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, boost::none);
    appendLiteral(expected, elem);
    appendEOO(expected);

    auto binData = skipCb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {BSONElement(), elem});
}

TEST_F(BSONColumnTest, BulkSkipConstructorValueAfterLarge) {
    size_t num = /*first non-RLE*/ 60 + /*RLE*/ 1920 * 12 + /*non-RLE at end*/ 59;
    auto elem = createElementInt32(1);
    BSONColumnBuilder<> skipCb(num);
    skipCb.append(elem);

    BufBuilder expected;
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks64(
        expected, std::vector<boost::optional<uint64_t>>(num - 1, boost::none), 16);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, std::vector<boost::optional<uint64_t>>(1, boost::none), 1);
    appendLiteral(expected, elem);
    appendEOO(expected);

    auto binData = skipCb.finalize();
    verifyBinary(binData, expected, false);
    verifyColumnReopenFromBinary(reinterpret_cast<const char*>(binData.data), binData.length);
}

TEST_F(BSONColumnTest, SimpleOneValueRLE) {
    // This test produces an RLE block after a literal.
    std::vector<BSONElement> elems;

    for (size_t i = 0; i < 121; ++i) {
        elems.push_back(createElementInt64(256));
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bRLE(expected, 120);
    appendEOO(expected);

    auto binData = cb.finalize();

    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, SkipRLEWithMoreSkips) {
    // This test is creating skips that's encoded as RLE where the first non-skip value is before
    // the RLE block.
    std::vector<BSONElement> elems = {createElementInt64(38), createElementInt64(40)};
    for (int i = 0; i < 140; ++i) {
        elems.push_back(BSONElement());
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0010);
    appendSimple8bBlocks64(
        expected, deltaInt64(elems.begin() + 1, elems.begin() + 21, elems.front()), 1);
    appendSimple8bRLE(expected, 120);
    appendSimple8bBlock64(expected, boost::none);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DateAllIdenticalRLENoOverflow) {
    // All identical values using date. Encoded as RLE that never overflow.
    std::vector<BSONElement> elems(simple8b_internal::kRleMultiplier + 1,
                                   createDate(Date_t::fromMillisSinceEpoch(1709224256429)));

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bRLE(expected, elems.size() - 1);
    appendEOO(expected);

    BSONBinData binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, RLEBeginningWithDifferentValAfterNoOverflow) {
    BSONElement e = createElementInt64(37);

    // Add enough elements to fill an RLE block
    std::vector<BSONElement> elems(simple8b_internal::kRleMultiplier + 1, e);

    // Add a different value after RLE.
    elems.push_back(createElementInt64(38));

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0001);
    appendSimple8bRLE(expected, simple8b_internal::kRleMultiplier);
    appendSimple8bBlock64(expected, deltaInt64(elems[elems.size() - 1], elems[elems.size() - 2]));
    appendEOO(expected);


    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}


TEST_F(BSONColumnTest, NonZeroRLETwoControlBlocks) {
    // This test validates handling when we have so many consecutive values with RLE blocks that
    // they span over two control blocks. We need to write at least 17 simple8b blocks for this to
    // be the case. As all values are the same the RLE blocks will need the max amount of values
    // which is 1920. When appending 1 deltas, we will first get a non-RLE block containing 30
    // values. Then we add 59 more at the end that need to be split up in 4 simple8b blocks. By
    // using 12 full RLE blocks we then get 17 simple8b blocks in total which require two bsoncolumn
    // control bytes.
    size_t num =
        /*uncompressed*/ 1 + /*first non-RLE*/ 30 + /*RLE*/ 1920 * 12 + /*non-RLE at end*/ 59;
    std::vector<BSONElement> elems;
    for (size_t i = 0; i < num; ++i) {
        elems.push_back(createElementInt32(i));
        cb.append(elems.back());
    }

    BufBuilder expected;
    appendLiteral(expected, createElementInt32(0));
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks64(
        expected,
        std::vector<boost::optional<uint64_t>>(num - 2, Simple8bTypeUtil::encodeInt64(1)),
        16);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected, std::vector<boost::optional<uint64_t>>(1, Simple8bTypeUtil::encodeInt64(1)), 1);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected, false);
    verifyDecompression(binData, elems);
    verifyColumnReopenFromBinary(reinterpret_cast<const char*>(binData.data), binData.length);
}

TEST_F(BSONColumnTest, RLEAfterMixedValueBlock) {
    // This test produces an RLE block after a simple8b block with different values. We test that we
    // can properly handle when the value to use for RLE is at the end of this block and not the
    // beginning.
    std::vector<BSONElement> elems = {createElementInt64(64), createElementInt64(128)};

    for (size_t i = 0; i < 128; ++i) {
        elems.push_back(createElementInt64(256));
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }


    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0001);
    appendSimple8bBlocks64(
        expected, deltaInt64(elems.begin() + 1, elems.begin() + 10, elems.front()), 1);
    appendSimple8bRLE(expected, 120);
    appendEOO(expected);

    auto binData = cb.finalize();

    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, RLEAfterMixedValueBlock128) {
    // This test produces an RLE block after a simple8b block with different values. We test that we
    // can properly handle when the value to use for RLE is at the end of this block and not the
    // beginning.

    // Generate strings from integer to make it easier to control the delta values
    auto createStringFromInt = [&](int64_t val) {
        auto str = Simple8bTypeUtil::decodeString(val);
        return createElementString(StringData(str.str.data(), str.size));
    };

    std::vector<BSONElement> elems = {createStringFromInt(64), createStringFromInt(128)};

    for (size_t i = 0; i < 128; ++i) {
        elems.push_back(createStringFromInt(256));
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }


    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0001);
    appendSimple8bBlocks128(
        expected, deltaString(elems.begin() + 1, elems.begin() + 10, elems.front()), 1);
    appendSimple8bRLE(expected, 120);
    appendEOO(expected);

    auto binData = cb.finalize();

    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, RLEFirstInControlAfterMixedValueBlock) {
    // This test produces an RLE block after a simple8b block with different values. The RLE block
    // is located as the first block after a control byte. We test that we can properly handle when
    // the value to use for RLE is at the end of the last block in the previous control byte and not
    // at the beginning of this block.
    std::vector<BSONElement> elems = {createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFF),
                                      createElementInt64(64),
                                      createElementInt64(128)};

    for (size_t i = 0; i < 129; ++i) {
        elems.push_back(createElementInt64(256));
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }


    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks64(
        expected,
        deltaInt64(elems.begin() + 1, elems.begin() + elems.size() - 120, elems.front()),
        16);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bRLE(expected, 120);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, RLEFirstInControlAfterMixedValueBlock128) {
    // This test produces an RLE block after a simple8b block with different values. The RLE block
    // is located as the first block after a control byte. We test that we can properly handle when
    // the value to use for RLE is at the end of the last block in the previous control byte and not
    // at the beginning of this block.

    // Generate strings from integer to make it easier to control the delta values
    auto createStringFromInt = [&](int64_t val) {
        auto str = Simple8bTypeUtil::decodeString(val);
        return createElementString(StringData(str.str.data(), str.size));
    };

    std::vector<BSONElement> elems = {createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFF),
                                      createStringFromInt(64),
                                      createStringFromInt(128)};

    for (size_t i = 0; i < 129; ++i) {
        elems.push_back(createStringFromInt(256));
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }


    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks128(
        expected,
        deltaString(elems.begin() + 1, elems.begin() + elems.size() - 120, elems.front()),
        16);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bRLE(expected, 120);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, RLEFirstInControlAfterMixedValueBlockWithMoreIdentical) {
    // This test produces an RLE block after a simple8b block with different values. The RLE block
    // is located as the first block after a control byte. In addition there are identical values to
    // the RLE block after that got written during finalize. We test that we can properly handle
    // when we have not yet overflowed in the RLE and must continue to search in the previous
    // control.
    std::vector<BSONElement> elems = {createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFF),
                                      createElementInt64(64),
                                      createElementInt64(128)};

    for (size_t i = 0; i < 130; ++i) {
        elems.push_back(createElementInt64(256));
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }


    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks64(
        expected,
        deltaInt64(elems.begin() + 1, elems.begin() + elems.size() - 121, elems.front()),
        16);
    appendSimple8bControl(expected, 0b1000, 0b0001);
    appendSimple8bRLE(expected, 120);
    appendSimple8bBlock64(expected, deltaInt64(elems.back(), *(elems.begin() + elems.size() - 2)));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, RLEFirstInControlAfterMixedValueBlockWithMoreIdentical128) {
    // This test produces an RLE block after a simple8b block with different values. The RLE block
    // is located as the first block after a control byte. In addition there are identical values to
    // the RLE block after that got written during finalize. We test that we can properly handle
    // when we have not yet overflowed in the RLE and must continue to search in the previous
    // control.

    // Generate strings from integer to make it easier to control the delta values
    auto createStringFromInt = [&](int64_t val) {
        auto str = Simple8bTypeUtil::decodeString(val);
        return createElementString(StringData(str.str.data(), str.size));
    };

    std::vector<BSONElement> elems = {createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFF),
                                      createStringFromInt(64),
                                      createStringFromInt(128)};

    for (size_t i = 0; i < 130; ++i) {
        elems.push_back(createStringFromInt(256));
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks128(
        expected,
        deltaString(elems.begin() + 1, elems.begin() + elems.size() - 121, elems.front()),
        16);
    appendSimple8bControl(expected, 0b1000, 0b0001);
    appendSimple8bRLE(expected, 120);
    appendSimple8bBlock128(expected,
                           deltaString(elems.back(), *(elems.begin() + elems.size() - 2)));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, RLEFirstInControlAfterMixedValueBlockWithMoreDifferent) {
    // This test produces an RLE block after a simple8b block with different values. The RLE block
    // is located as the first block after a control byte. In addition there are identical values to
    // the RLE block after that got written during finalize. We test that we can properly handle
    // when we have not yet overflowed in the RLE and must continue to search in the previous
    // control.
    std::vector<BSONElement> elems = {createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFF),
                                      createElementInt64(64),
                                      createElementInt64(128)};

    for (size_t i = 0; i < 129; ++i) {
        elems.push_back(createElementInt64(256));
    }

    elems.push_back(createElementInt64(0));

    for (auto&& elem : elems) {
        cb.append(elem);
    }


    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks64(
        expected,
        deltaInt64(elems.begin() + 1, elems.begin() + elems.size() - 121, elems.front()),
        16);
    appendSimple8bControl(expected, 0b1000, 0b0001);
    appendSimple8bRLE(expected, 120);
    appendSimple8bBlock64(expected, deltaInt64(elems.back(), *(elems.begin() + elems.size() - 2)));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, RLEFirstInControlAfterMixedValueBlockWithMoreDifferent128) {
    // This test produces an RLE block after a simple8b block with different values. The RLE block
    // is located as the first block after a control byte. In addition there are identical values to
    // the RLE block after that got written during finalize. We test that we can properly handle
    // when we have not yet overflowed in the RLE and must continue to search in the previous
    // control.

    // Generate strings from integer to make it easier to control the delta values
    auto createStringFromInt = [&](int64_t val) {
        auto str = Simple8bTypeUtil::decodeString(val);
        return createElementString(StringData(str.str.data(), str.size));
    };

    std::vector<BSONElement> elems = {createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFFFFFFFFFFFF),
                                      createStringFromInt(0),
                                      createStringFromInt(0xFFFF),
                                      createStringFromInt(64),
                                      createStringFromInt(128)};

    for (size_t i = 0; i < 129; ++i) {
        elems.push_back(createStringFromInt(256));
    }

    elems.push_back(createStringFromInt(0));

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks128(
        expected,
        deltaString(elems.begin() + 1, elems.begin() + elems.size() - 121, elems.front()),
        16);
    appendSimple8bControl(expected, 0b1000, 0b0001);
    appendSimple8bRLE(expected, 120);
    appendSimple8bBlock128(expected,
                           deltaString(elems.back(), *(elems.begin() + elems.size() - 2)));
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, RLEFirstInControlWithNonRLEAfterWithoutOverflow) {
    // This test has a control block beginning with RLE and non-RLE data after the RLE that doesn't
    // overflow by itself. Test that we properly detect the overflow to be inside the RLE block.
    std::vector<BSONElement> elems = {createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFFFFFFFFFFFF),
                                      createElementInt64(0),
                                      createElementInt64(0xFFFF),
                                      createElementInt64(64),
                                      createElementInt64(128)};

    for (size_t i = 0; i < 190; ++i) {
        elems.push_back(BSONElement());
    }

    elems.push_back(createElementInt64(128));

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks64(
        expected,
        deltaInt64(elems.begin() + 1, elems.begin() + elems.size() - 131, elems.front()),
        16);
    appendSimple8bControl(expected, 0b1000, 0b0010);
    appendSimple8bRLE(expected, 120);
    appendSimple8bBlocks64(
        expected,
        deltaInt64(elems.begin() + elems.size() - 11, elems.begin() + elems.size(), elems[17]),
        2);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected, true);
    verifyDecompression(binData, elems);
}


TEST_F(BSONColumnTest, RLELargeValueExtendedSelector) {
    // This test creates an RLE block containing large values that do not fit in the base selector.
    // Ensure the correct selector states are set for binary reopen of this binary.
    uint64_t val = 0;
    uint64_t delta = 0x3F00000000000000;
    std::vector<BSONElement> elems;
    // Add extra value so they are not all the same
    elems.push_back(createElementInt64(val));
    for (int i = 0; i < 125; ++i, val += delta) {
        elems.push_back(createElementInt64(val));
    }

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0001);
    appendSimple8bBlocks64(
        expected, deltaInt64(elems.begin() + 1, elems.begin() + 6, elems.front()), 1);
    appendSimple8bRLE(expected, 120);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, PendingRLECreateNewControlAtFinalize) {
    BSONColumnBuilder cb;

    // This test has multiple pending RLE eligible values that are flushed out when a non RLE
    // eligible value is appended at the end. This cause a new control block to be written. Make
    // sure intermediate can return correct diffs in this case.
    std::vector<BSONElement> elems = {createElementInt64(0)};
    elems.insert(elems.end(), 7, createElementInt64(2));
    elems.insert(elems.end(), 1, createElementInt64(65282));
    elems.insert(elems.end(), 30, createElementInt64(2));
    elems.insert(elems.end(), 1, createElementInt64(258));
    elems.insert(elems.end(), 96, createElementInt64(2));
    elems.insert(elems.end(), 1, createElementInt64(65282));
    elems.insert(elems.end(), 34, createElementInt64(2));
    elems.insert(elems.end(), 1, createElementInt64(258));
    elems.insert(elems.end(), 92, createElementInt64(2));
    elems.insert(elems.end(), 1, createElementInt64(65282));
    elems.insert(elems.end(), 117, createElementInt64(2));
    elems.insert(elems.end(), 1, createElementInt64(258));

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b1111);
    appendSimple8bBlocks64(
        expected, deltaInt64(elems.begin() + 1, elems.begin() + 356, elems.front()), 16);
    appendSimple8bControl(expected, 0b1000, 0b0001);
    appendSimple8bBlocks64(
        expected, deltaInt64(elems.begin() + 356, elems.end(), elems.at(355)), 2);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DefaultSelectorAfterExtended) {
    // This test is having a large delta that must be stored in the extended selectors, after comes
    // a small value. We need to properly adjust selector state when reopening.
    std::vector<BSONElement> elems = {
        createElementString("core"_sd), createElementString("Singapore"_sd), BSONElement()};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0001);
    appendSimple8bBlocks128(
        expected, deltaString(elems.begin() + 1, elems.end(), elems.front()), 2);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, Interleaved) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << 2)),
                                      createElementObj(BSON("x" << 1 << "y" << 3)),
                                      createElementObj(BSON("x" << 1 << "y" << 3)),
                                      BSONElement(),
                                      createElementObj(BSON("y" << 4)),
                                      createElementObj(BSON("x" << 1 << "y" << 3))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                            deltaInt32(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd]),
                            boost::none,
                            boost::none,
                            deltaInt32(elems[5].Obj()["x"_sd], elems[2].Obj()["x"_sd])},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd]),
                            deltaInt32(elems[2].Obj()["y"_sd], elems[1].Obj()["y"_sd]),
                            boost::none,
                            deltaInt32(elems[4].Obj()["y"_sd], elems[2].Obj()["y"_sd]),
                            deltaInt32(elems[5].Obj()["y"_sd], elems[4].Obj()["y"_sd])},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << 2)),
                                      createElementObj(BSON("x" << 1 << "y" << 3)),
                                      createElementObj(BSON("x" << 1 << "y" << 3)),
                                      BSONElement(),
                                      createElementObj(BSON("y" << 4)),
                                      createElementObj(BSON("x" << 1 << "y" << 3))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                            deltaInt32(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd]),
                            boost::none,
                            boost::none,
                            deltaInt32(elems[5].Obj()["x"_sd], elems[2].Obj()["x"_sd])},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd]),
                            deltaInt32(elems[2].Obj()["y"_sd], elems[1].Obj()["y"_sd]),
                            boost::none,
                            deltaInt32(elems[4].Obj()["y"_sd], elems[2].Obj()["y"_sd]),
                            deltaInt32(elems[5].Obj()["y"_sd], elems[4].Obj()["y"_sd])},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedArray) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << BSON_ARRAY(1 << 2))),
                                      createElementObj(BSON("x" << BSON_ARRAY(2 << 3)))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaInt32(elems[1].Obj()["x"_sd].Array()[0], elems[0].Obj()["x"_sd].Array()[0])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaInt32(elems[1].Obj()["x"_sd].Array()[1], elems[0].Obj()["x"_sd].Array()[1])},
        1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedArrayRoot) {
    std::vector<BSONElement> elems = {createElementArray(BSON_ARRAY(1 << 2)),
                                      createElementArray(BSON_ARRAY(2 << 3))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStartArrayRoot(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Array()[0], elems[0].Array()[0])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Array()[1], elems[0].Array()[1])},
        1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedArrayRootTypeChange) {
    std::vector<BSONElement> elems = {createElementArray(BSON_ARRAY(1 << 2)),
                                      createElementArrayAsObject(BSON_ARRAY(2 << 3))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStartArrayRoot(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[1].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedAfterNonInterleaved) {
    std::vector<BSONElement> elems = {createElementInt32(1),
                                      createElementObj(BSON("x" << 1 << "y" << 2)),
                                      createElementObj(BSON("x" << 1 << "y" << 3)),
                                      createElementObj(BSON("x" << 2 << "y" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendInterleavedStart(expected, elems[1].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd]),
                            deltaInt32(elems[3].Obj()["x"_sd], elems[2].Obj()["x"_sd])},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[2].Obj()["y"_sd], elems[1].Obj()["y"_sd]),
                            deltaInt32(elems[3].Obj()["y"_sd], elems[2].Obj()["y"_sd])},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedAfterNonInterleavedLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementInt32(1),
                                      createElementObj(BSON("x" << 1 << "y" << 2)),
                                      createElementObj(BSON("x" << 1 << "y" << 3)),
                                      createElementObj(BSON("x" << 2 << "y" << 4))};

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendInterleavedStartLegacy(expected, elems[1].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd]),
                            deltaInt32(elems[3].Obj()["x"_sd], elems[2].Obj()["x"_sd])},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[2].Obj()["y"_sd], elems[1].Obj()["y"_sd]),
                            deltaInt32(elems[3].Obj()["y"_sd], elems[2].Obj()["y"_sd])},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedLevels) {
    std::vector<BSONElement> elems = {createElementObj(BSON("root" << BSON("x" << 1) << "y" << 2)),
                                      createElementObj(BSON("root" << BSON("x" << 2) << "y" << 5)),
                                      createElementObj(BSON("root" << BSON("x" << 2) << "y" << 5))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["root"_sd].Obj()["x"_sd],
                                       elems[0].Obj()["root"_sd].Obj()["x"_sd]),
                            deltaInt32(elems[2].Obj()["root"_sd].Obj()["x"_sd],
                                       elems[1].Obj()["root"_sd].Obj()["x"_sd])},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd]),
                            deltaInt32(elems[2].Obj()["y"_sd], elems[1].Obj()["y"_sd])},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedLevelsLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementObj(BSON("root" << BSON("x" << 1) << "y" << 2)),
                                      createElementObj(BSON("root" << BSON("x" << 2) << "y" << 5)),
                                      createElementObj(BSON("root" << BSON("x" << 2) << "y" << 5))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["root"_sd].Obj()["x"_sd],
                                       elems[0].Obj()["root"_sd].Obj()["x"_sd]),
                            deltaInt32(elems[2].Obj()["root"_sd].Obj()["x"_sd],
                                       elems[1].Obj()["root"_sd].Obj()["x"_sd])},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd]),
                            deltaInt32(elems[2].Obj()["y"_sd], elems[1].Obj()["y"_sd])},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedDoubleDifferentScale) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1.0 << "y" << 2.0)),
                                      createElementObj(BSON("x" << 1.1 << "y" << 3.0)),
                                      createElementObj(BSON("x" << 1.2 << "y" << 2.0)),
                                      createElementObj(BSON("x" << 1.0)),
                                      createElementObj(BSON("x" << 1.5 << "y" << 2.0))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1010, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaDouble(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd], 10),
                            deltaDouble(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd], 10),
                            deltaDouble(elems[3].Obj()["x"_sd], elems[2].Obj()["x"_sd], 10),
                            deltaDouble(elems[4].Obj()["x"_sd], elems[3].Obj()["x"_sd], 10)},
                           1);
    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaDouble(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd], 1),
                            deltaDouble(elems[2].Obj()["y"_sd], elems[1].Obj()["y"_sd], 1),
                            boost::none,
                            deltaDouble(elems[4].Obj()["y"_sd], elems[2].Obj()["y"_sd], 1)},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);

    TestPath testPathX{{"x"}};
    verifyDecompressPathFast(binData, elems, testPathX);

    TestPath testPathY{{"y"}};
    verifyDecompressPathFast(binData, elems, testPathY);
}

TEST_F(BSONColumnTest, InterleavedDoubleDifferentScaleLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1.0 << "y" << 2.0)),
                                      createElementObj(BSON("x" << 1.1 << "y" << 3.0)),
                                      createElementObj(BSON("x" << 1.2 << "y" << 2.0)),
                                      createElementObj(BSON("x" << 1.0)),
                                      createElementObj(BSON("x" << 1.5 << "y" << 2.0))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1010, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaDouble(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd], 10),
                            deltaDouble(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd], 10),
                            deltaDouble(elems[3].Obj()["x"_sd], elems[2].Obj()["x"_sd], 10),
                            deltaDouble(elems[4].Obj()["x"_sd], elems[3].Obj()["x"_sd], 10)},
                           1);
    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaDouble(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd], 1),
                            deltaDouble(elems[2].Obj()["y"_sd], elems[1].Obj()["y"_sd], 1),
                            boost::none,
                            deltaDouble(elems[4].Obj()["y"_sd], elems[2].Obj()["y"_sd], 1)},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedDoubleIncreaseScaleFromDeltaNoRescale) {
    std::vector<BSONElement> elems;
    elems.push_back(createElementObj(BSON("x" << 1.1)));
    elems.push_back(createElementObj(BSON("x" << 2.1)));
    elems.push_back(createElementObj(BSON("x" << 2.2)));
    elems.push_back(createElementObj(BSON("x" << 2.3)));
    elems.push_back(createElementObj(BSON("x" << 3.12345678)));

    for (const auto& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());

    appendSimple8bControl(expected, 0b1010, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaDouble(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd], 10),
                            deltaDouble(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd], 10),
                            deltaDouble(elems[3].Obj()["x"_sd], elems[2].Obj()["x"_sd], 10)},
                           1);
    appendSimple8bControl(expected, 0b1101, 0b0000);
    appendSimple8bBlocks64(
        expected, {deltaDouble(elems[4].Obj()["x"_sd], elems[3].Obj()["x"_sd], 100000000)}, 1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(expected, elems);
    verifyDecompressPathFast(binData, elems, TestPath{{"x"}});
}

TEST_F(BSONColumnTest, InterleavedDoubleIncreaseScaleFromDeltaNoRescaleLegacyDecompress) {
    std::vector<BSONElement> elems;
    elems.push_back(createElementObj(BSON("x" << 1.1)));
    elems.push_back(createElementObj(BSON("x" << 2.1)));
    elems.push_back(createElementObj(BSON("x" << 2.2)));
    elems.push_back(createElementObj(BSON("x" << 2.3)));
    elems.push_back(createElementObj(BSON("x" << 3.12345678)));

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());

    appendSimple8bControl(expected, 0b1010, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaDouble(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd], 10),
                            deltaDouble(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd], 10),
                            deltaDouble(elems[3].Obj()["x"_sd], elems[2].Obj()["x"_sd], 10)},
                           1);
    appendSimple8bControl(expected, 0b1101, 0b0000);
    appendSimple8bBlocks64(
        expected, {deltaDouble(elems[4].Obj()["x"_sd], elems[3].Obj()["x"_sd], 100000000)}, 1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}};
    verifyDecompressPathFast(expected, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedScalarToObject) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                      createElementObj(BSON("x" << 2)),
                                      createElementObj(BSON("x" << BSON("y" << 1 << "z" << 1))),
                                      createElementObj(BSON("x" << BSON("y" << 2 << "z" << 3)))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaInt32(elems[3].Obj()["x"_sd].Obj()["y"_sd], elems[2].Obj()["x"_sd].Obj()["y"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaInt32(elems[3].Obj()["x"_sd].Obj()["z"_sd], elems[2].Obj()["x"_sd].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, ArrayThenObjectNoScalars) {
    std::vector<BSONElement> elems = {
        createElementArray(BSON_ARRAY(1 << BSONObjBuilder().obj() << 2)),
        createElementObj(BSON("x" << BSONObjBuilder().obj()))};

    for (auto elem : elems) {
        cb.append(elem);
    }
    auto binData = cb.finalize();

    BufBuilder expected;
    appendInterleavedStartArrayRoot(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendLiteral(expected, elems[1]);
    appendEOO(expected);

    verifyBinary(binData, expected);
    verifyDecompression(binData, elems, false);
}

TEST_F(BSONColumnTest, InterleavedScalarToObjectLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                      createElementObj(BSON("x" << 2)),
                                      createElementObj(BSON("x" << BSON("y" << 1 << "z" << 1))),
                                      createElementObj(BSON("x" << BSON("y" << 2 << "z" << 3)))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaInt32(elems[3].Obj()["x"_sd].Obj()["y"_sd], elems[2].Obj()["x"_sd].Obj()["y"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaInt32(elems[3].Obj()["x"_sd].Obj()["z"_sd], elems[2].Obj()["x"_sd].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, DecodeInterleavedObjectAsScalar) {
    auto test = [&](auto appendInterleavedStartFunc) {
        // Verify that we can decompress the following elements when sub-objects are encoded as
        // scalars in a single interleaved stream
        std::vector<BSONElement> elems = {
            createElementObj(BSON("x" << 1)),
            createElementObj(BSON("x" << 2)),
            createElementObj(BSON("x" << BSON("y" << 1 << "z" << 1))),
            createElementObj(BSON("x" << BSON("y" << 2 << "z" << 3))),
            createElementObj(BSON("x" << BSON("y" << 2 << "z" << 3)))};

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendLiteral(expected, elems[2].Obj()["x"_sd]);
        appendLiteral(expected, elems[3].Obj()["x"_sd]);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);
        appendEOO(expected);

        BSONBinData binData;
        binData.data = expected.buf();
        binData.length = expected.len();
        binData.type = BinDataType::Column;
        verifyDecompression(binData, elems);
    };

    test(appendInterleavedStartLegacy);
    test(appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedMix64And128Bit) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y"
                                                                << "count0")),
                                      createElementObj(BSON("x" << 2 << "y"
                                                                << "count1")),
                                      createElementObj(BSON("x" << 3 << "y"
                                                                << "count2")),
                                      createElementObj(BSON("x" << 4)),
                                      createElementObj(BSON("x" << 5 << "y"
                                                                << "count3"))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                            deltaInt32(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd]),
                            deltaInt32(elems[3].Obj()["x"_sd], elems[2].Obj()["x"_sd]),
                            deltaInt32(elems[4].Obj()["x"_sd], elems[3].Obj()["x"_sd])},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks128(expected,
                            {kDeltaForBinaryEqualValues128,
                             deltaString(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd]),
                             deltaString(elems[2].Obj()["y"_sd], elems[1].Obj()["y"_sd]),
                             boost::none,
                             deltaString(elems[4].Obj()["y"_sd], elems[2].Obj()["y"_sd])},
                            1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedMix64And128BitLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y"
                                                                << "count0")),
                                      createElementObj(BSON("x" << 2 << "y"
                                                                << "count1")),
                                      createElementObj(BSON("x" << 3 << "y"
                                                                << "count2")),
                                      createElementObj(BSON("x" << 4)),
                                      createElementObj(BSON("x" << 5 << "y"
                                                                << "count3"))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                            deltaInt32(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd]),
                            deltaInt32(elems[3].Obj()["x"_sd], elems[2].Obj()["x"_sd]),
                            deltaInt32(elems[4].Obj()["x"_sd], elems[3].Obj()["x"_sd])},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks128(expected,
                            {kDeltaForBinaryEqualValues128,
                             deltaString(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd]),
                             deltaString(elems[2].Obj()["y"_sd], elems[1].Obj()["y"_sd]),
                             boost::none,
                             deltaString(elems[4].Obj()["y"_sd], elems[2].Obj()["y"_sd])},
                            1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedWithEmptySubObj) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj())),
        createElementObj(BSON("x" << 2 << "y" << BSONObjBuilder().obj())),
        createElementObj(BSON("x" << 3 << "y" << BSONObjBuilder().obj()))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                            deltaInt32(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd])},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}, TestPath{{"y"}}};
    verifyDecompressPathFast(binData, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedWithEmptySubObjLegacyDecompress) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj())),
        createElementObj(BSON("x" << 2 << "y" << BSONObjBuilder().obj())),
        createElementObj(BSON("x" << 3 << "y" << BSONObjBuilder().obj()))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                            deltaInt32(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd])},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}, TestPath{{"y"}}};
    verifyDecompressPathFast(expected, elems, testPaths);
}

TEST_F(BSONColumnTest, ObjectLiteralWithNoLeaves) {
    BSONElement elem = createElementObj(BSON("x" << BSONObj{}));
    cb.append(elem);

    // Path decompressor should find nothing for path "x"
    std::vector<BSONElement> expectedElems = {createElementObj({})};

    auto binData = cb.finalize();

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendEOO(expected);

    verifyBinary(binData, expected);
    std::vector<TestPath> testPaths{
        TestPath{{"x"}},
        TestPath{{"y"}},
    };
    verifyDecompressPathFast(binData, expectedElems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedRemoveEmptySubObj) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj())),
        createElementObj(BSON("x" << 2 << "y" << BSONObjBuilder().obj())),
        createElementObj(BSON("x" << 3))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}, TestPath{{"y"}}};
    verifyDecompressPathFast(binData, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedRemoveEmptySubObjLegacyDecompress) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj())),
        createElementObj(BSON("x" << 2 << "y" << BSONObjBuilder().obj())),
        createElementObj(BSON("x" << 3))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}, TestPath{{"y"}}};
    verifyDecompressPathFast(expected, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedAddEmptySubObj) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 3)),
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj()))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[1].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}, TestPath{{"y"}}};
    verifyDecompressPathFast(binData, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedAddEmptySubObjLegacyDecompress) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 3)),
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj()))};


    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[1].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}, TestPath{{"y"}}};
    verifyDecompressPathFast(expected, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedAddEmptySubArray) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 3)),
        createElementObj(BSON("x" << 1 << "y" << BSONArrayBuilder().arr()))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[1].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}, TestPath{{"y"}}};
    verifyDecompressPathFast(binData, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedSchemaChange) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << 2)),
                                      createElementObj(BSON("x" << 1 << "y" << 3)),
                                      createElementObj(BSON("x" << 1 << "y" << 3.0)),
                                      createElementObj(BSON("x" << 1 << "y" << 4.0))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                            deltaInt32(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd]),
                            deltaInt32(elems[3].Obj()["x"_sd], elems[2].Obj()["x"_sd])},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendLiteral(expected, elems[2].Obj()["y"_sd]);
    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bBlocks64(
        expected, {deltaDouble(elems[3].Obj()["y"_sd], elems[2].Obj()["y"_sd], 1)}, 1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedSchemaChangeLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << 2)),
                                      createElementObj(BSON("x" << 1 << "y" << 3)),
                                      createElementObj(BSON("x" << 1 << "y" << 3.0)),
                                      createElementObj(BSON("x" << 1 << "y" << 4.0))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                            deltaInt32(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd]),
                            deltaInt32(elems[3].Obj()["x"_sd], elems[2].Obj()["x"_sd])},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendLiteral(expected, elems[2].Obj()["y"_sd]);
    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bBlocks64(
        expected, {deltaDouble(elems[3].Obj()["y"_sd], elems[2].Obj()["y"_sd], 1)}, 1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectSchemaChange) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << BSON("z" << 2))),
                                      createElementObj(BSON("x" << 1 << "y" << BSON("z" << 3))),
                                      createElementObj(BSON("x" << 1 << "y" << 3))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaInt32(elems[1].Obj()["y"_sd].Obj()["z"_sd], elems[0].Obj()["y"_sd].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectSchemaChangeLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << BSON("z" << 2))),
                                      createElementObj(BSON("x" << 1 << "y" << BSON("z" << 3))),
                                      createElementObj(BSON("x" << 1 << "y" << 3))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaInt32(elems[1].Obj()["y"_sd].Obj()["z"_sd], elems[0].Obj()["y"_sd].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectNameChange) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << BSON("z" << 2))),
                                      createElementObj(BSON("x" << 1 << "y2" << BSON("z" << 3)))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected,
                           BSON("x" << 1 << "y" << BSON("z" << 2) << "y2" << BSON("z" << 3)));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues, boost::none}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {boost::none, kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectNameChangeLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << BSON("z" << 2))),
                                      createElementObj(BSON("x" << 1 << "y2" << BSON("z" << 3)))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected,
                                 BSON("x" << 1 << "y" << BSON("z" << 2) << "y2" << BSON("z" << 3)));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues, boost::none}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {boost::none, kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectEmptyObjChange) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSON("z" << 2))),
        createElementObj(BSON("x" << 1 << "y" << BSON("z" << 3))),
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj()))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaInt32(elems[1].Obj()["y"_sd].Obj()["z"_sd], elems[0].Obj()["y"_sd].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    verifyDecompressPathFast(binData, elems, TestPath{{"y", "z"}});
}

TEST_F(BSONColumnTest, InterleavedObjectEmptyObjChangeLegacyDecompress) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSON("z" << 2))),
        createElementObj(BSON("x" << 1 << "y" << BSON("z" << 3))),
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj()))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaInt32(elems[1].Obj()["y"_sd].Obj()["z"_sd], elems[0].Obj()["y"_sd].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
    std::vector<TestPath> testPaths{TestPath{{"y", "z"}}};
    verifyDecompressPathFast(expected, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedObjectEmptyArrayChange) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSON("z" << 2))),
        createElementObj(BSON("x" << 1 << "y" << BSON("z" << 3))),
        createElementObj(BSON("x" << 1 << "y" << BSONArrayBuilder().arr()))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaInt32(elems[1].Obj()["y"_sd].Obj()["z"_sd], elems[0].Obj()["y"_sd].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    verifyDecompressPathFast(binData, elems, TestPath{{"y", "z"}});
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyObjMiddle) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "z" << 2)),
        createElementObj(BSON("x" << 1 << "z" << 3)),
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj() << "z" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    verifyDecompressPathFast(binData, elems, TestPath{{"y"}});
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyObjMiddleLegacyDecompress) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "z" << 2)),
        createElementObj(BSON("x" << 1 << "z" << 3)),
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj() << "z" << 4))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
    std::vector<TestPath> testPaths{TestPath{{"y"}}};
    verifyDecompressPathFast(expected, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyArrayMiddle) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "z" << 2)),
        createElementObj(BSON("x" << 1 << "z" << 3)),
        createElementObj(BSON("x" << 1 << "y" << BSONArrayBuilder().arr() << "z" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    verifyDecompressPathFast(binData, elems, TestPath{{"y"}});
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyObjUnderObj) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "z" << 2)),
        createElementObj(BSON("x" << 1 << "z" << 3)),
        createElementObj(
            BSON("x" << 1 << "y" << BSON("y1" << BSONObjBuilder().obj()) << "z" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyObjUnderObjLegacyDecompress) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "z" << 2)),
        createElementObj(BSON("x" << 1 << "z" << 3)),
        createElementObj(
            BSON("x" << 1 << "y" << BSON("y1" << BSONObjBuilder().obj()) << "z" << 4))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyArrayUnderObj) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "z" << 2)),
        createElementObj(BSON("x" << 1 << "z" << 3)),
        createElementObj(
            BSON("x" << 1 << "y" << BSON("y1" << BSONArrayBuilder().arr()) << "z" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyObjEnd) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2)),
        createElementObj(BSON("x" << 1 << "y" << 3)),
        createElementObj(BSON("x" << 1 << "y" << 4 << "z" << BSONObjBuilder().obj()))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyObjEndLegacyDecompress) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2)),
        createElementObj(BSON("x" << 1 << "y" << 3)),
        createElementObj(BSON("x" << 1 << "y" << 4 << "z" << BSONObjBuilder().obj()))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyArrayEnd) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2)),
        createElementObj(BSON("x" << 1 << "y" << 3)),
        createElementObj(BSON("x" << 1 << "y" << 4 << "z" << BSONArrayBuilder().arr()))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyObjUnderObjEnd) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2)),
        createElementObj(BSON("x" << 1 << "y" << 3)),
        createElementObj(
            BSON("x" << 1 << "y" << 4 << "z" << BSON("z1" << BSONObjBuilder().obj())))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyObjUnderObjEndLegacyDecompress) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2)),
        createElementObj(BSON("x" << 1 << "y" << 3)),
        createElementObj(
            BSON("x" << 1 << "y" << 4 << "z" << BSON("z1" << BSONObjBuilder().obj())))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyArrayUnderObjEnd) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2)),
        createElementObj(BSON("x" << 1 << "y" << 3)),
        createElementObj(
            BSON("x" << 1 << "y" << 4 << "z" << BSON("z1" << BSONArrayBuilder().arr())))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyArrayUnderArrayEnd) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2)),
        createElementObj(BSON("x" << 1 << "y" << 3)),
        createElementObj(
            BSON("x" << 1 << "y" << 4 << "z" << BSON_ARRAY(BSONArrayBuilder().arr())))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyObjMiddle) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj() << "z" << 2)),
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj() << "z" << 3)),
        createElementObj(BSON("x" << 1 << "z" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyObjMiddleLegacyDecompress) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj() << "z" << 2)),
        createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj() << "z" << 3)),
        createElementObj(BSON("x" << 1 << "z" << 4))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyArrayMiddle) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSONArrayBuilder().arr() << "z" << 2)),
        createElementObj(BSON("x" << 1 << "y" << BSONArrayBuilder().arr() << "z" << 3)),
        createElementObj(BSON("x" << 1 << "z" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedArrayMissingEmptyObjMiddle) {
    std::vector<BSONElement> elems = {
        createElementArray(BSON_ARRAY(1 << BSONObjBuilder().obj() << 2)),
        createElementArray(BSON_ARRAY(1 << BSONObjBuilder().obj() << 3)),
        createElementArray(BSON_ARRAY(1 << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStartArrayRoot(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Array()[0], elems[0].Array()[0])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Array()[2], elems[0].Array()[2])},
        1);
    appendEOO(expected);

    appendInterleavedStartArrayRoot(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyObjUnderObj) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSON("y1" << BSONObjBuilder().obj()) << "z" << 2)),
        createElementObj(BSON("x" << 1 << "y" << BSON("y1" << BSONObjBuilder().obj()) << "z" << 3)),
        createElementObj(BSON("x" << 1 << "z" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyObjUnderObjLegacyDecompress) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSON("y1" << BSONObjBuilder().obj()) << "z" << 2)),
        createElementObj(BSON("x" << 1 << "y" << BSON("y1" << BSONObjBuilder().obj()) << "z" << 3)),
        createElementObj(BSON("x" << 1 << "z" << 4))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyArrayUnderObj) {
    std::vector<BSONElement> elems = {
        createElementObj(
            BSON("x" << 1 << "y" << BSON("y1" << BSONArrayBuilder().arr()) << "z" << 2)),
        createElementObj(
            BSON("x" << 1 << "y" << BSON("y1" << BSONArrayBuilder().arr()) << "z" << 3)),
        createElementObj(BSON("x" << 1 << "z" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyArrayUnderArray) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSON_ARRAY(BSONArrayBuilder().arr()) << "z" << 2)),
        createElementObj(BSON("x" << 1 << "y" << BSON_ARRAY(BSONArrayBuilder().arr()) << "z" << 3)),
        createElementObj(BSON("x" << 1 << "z" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyObjEnd) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2 << "z" << BSONObjBuilder().obj())),
        createElementObj(BSON("x" << 1 << "y" << 3 << "z" << BSONObjBuilder().obj())),
        createElementObj(BSON("x" << 1 << "y" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyObjEndLegacyDecompress) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2 << "z" << BSONObjBuilder().obj())),
        createElementObj(BSON("x" << 1 << "y" << 3 << "z" << BSONObjBuilder().obj())),
        createElementObj(BSON("x" << 1 << "y" << 4))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyArrayEnd) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2 << "z" << BSONArrayBuilder().arr())),
        createElementObj(BSON("x" << 1 << "y" << 3 << "z" << BSONArrayBuilder().arr())),
        createElementObj(BSON("x" << 1 << "y" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedArrayMissingEmptyArrayEnd) {
    std::vector<BSONElement> elems = {
        createElementArray(BSON_ARRAY(1 << 2 << BSONArrayBuilder().arr())),
        createElementArray(BSON_ARRAY(1 << 3 << BSONArrayBuilder().arr())),
        createElementArray(BSON_ARRAY(1 << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStartArrayRoot(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Array()[0], elems[0].Array()[0])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Array()[1], elems[0].Array()[1])},
        1);
    appendEOO(expected);

    appendInterleavedStartArrayRoot(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyObjUnderObjEnd) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2 << "z" << BSON("z1" << BSONObjBuilder().obj()))),
        createElementObj(BSON("x" << 1 << "y" << 3 << "z" << BSON("z1" << BSONObjBuilder().obj()))),
        createElementObj(BSON("x" << 1 << "y" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    verifyDecompressPathFast(binData, elems, TestPath{{"z", "z1"}});
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyObjUnderObjEndLegacyDecompress) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2 << "z" << BSON("z1" << BSONObjBuilder().obj()))),
        createElementObj(BSON("x" << 1 << "y" << 3 << "z" << BSON("z1" << BSONObjBuilder().obj()))),
        createElementObj(BSON("x" << 1 << "y" << 4))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
    std::vector<TestPath> testPaths{TestPath{{"z", "z1"}}};
    verifyDecompressPathFast(expected, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyArrayUnderObjEnd) {
    std::vector<BSONElement> elems = {
        createElementObj(
            BSON("x" << 1 << "y" << 2 << "z" << BSON("z1" << BSONArrayBuilder().arr()))),
        createElementObj(
            BSON("x" << 1 << "y" << 3 << "z" << BSON("z1" << BSONArrayBuilder().arr()))),
        createElementObj(BSON("x" << 1 << "y" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    verifyDecompressPathFast(binData, elems, TestPath{{"z", "z1"}});
}

TEST_F(BSONColumnTest, InterleavedArrayMissingEmptyObjUnderObjEnd) {
    std::vector<BSONElement> elems = {
        createElementArray(BSON_ARRAY(1 << 2 << BSON("z1" << BSONObjBuilder().obj()))),
        createElementArray(BSON_ARRAY(1 << 3 << BSON("z1" << BSONObjBuilder().obj()))),
        createElementArray(BSON_ARRAY(1 << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStartArrayRoot(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Array()[0], elems[0].Array()[0])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Array()[1], elems[0].Array()[1])},
        1);
    appendEOO(expected);

    appendInterleavedStartArrayRoot(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, ReenterInterleaved) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << 2)),
                                      createElementObj(BSON("x" << 1 << "y" << 3)),
                                      createElementInt32(1),
                                      createElementObj(BSON("x" << 2 << "y" << 2 << "z" << 2)),
                                      createElementObj(BSON("x" << 5 << "y" << 3 << "z" << 3))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);
    appendLiteral(expected, elems[2]);
    appendInterleavedStart(expected, elems[3].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[4].Obj()["x"_sd], elems[3].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[4].Obj()["y"_sd], elems[3].Obj()["y"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[4].Obj()["z"_sd], elems[3].Obj()["z"_sd])},
        1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, ReenterInterleavedLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << 2)),
                                      createElementObj(BSON("x" << 1 << "y" << 3)),
                                      createElementInt32(1),
                                      createElementObj(BSON("x" << 2 << "y" << 2 << "z" << 2)),
                                      createElementObj(BSON("x" << 5 << "y" << 3 << "z" << 3))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
        1);
    appendEOO(expected);
    appendLiteral(expected, elems[2]);
    appendInterleavedStartLegacy(expected, elems[3].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[4].Obj()["x"_sd], elems[3].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[4].Obj()["y"_sd], elems[3].Obj()["y"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[4].Obj()["z"_sd], elems[3].Obj()["z"_sd])},
        1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, ReenterInterleavedArrayRootToObj) {
    std::vector<BSONElement> elems = {createElementArray(BSON_ARRAY(1 << 2)),
                                      createElementArray(BSON_ARRAY(1 << 3)),
                                      createElementInt32(1),
                                      createElementObj(BSON("x" << 2 << "y" << 2 << "z" << 2)),
                                      createElementObj(BSON("x" << 5 << "y" << 3 << "z" << 3))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStartArrayRoot(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Array()[0], elems[0].Array()[0])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Array()[1], elems[0].Array()[1])},
        1);
    appendEOO(expected);
    appendLiteral(expected, elems[2]);
    appendInterleavedStart(expected, elems[3].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[4].Obj()["x"_sd], elems[3].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[4].Obj()["y"_sd], elems[3].Obj()["y"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[4].Obj()["z"_sd], elems[3].Obj()["z"_sd])},
        1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedAlternatingMergeRight) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                      createElementObj(BSON("y" << 2)),
                                      createElementObj(BSON("z" << 3)),
                                      createElementObj(BSON("x" << 2)),
                                      createElementObj(BSON("y" << 3)),
                                      createElementObj(BSON("z" << 4))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected,
                           BSON("x" << elems[0].Obj().firstElement().Int() << "y"
                                    << elems[1].Obj().firstElement().Int() << "z"
                                    << elems[2].Obj().firstElement().Int()));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            boost::none,
                            boost::none,
                            deltaInt32(elems[3].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                            boost::none,
                            boost::none},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {boost::none,
                            kDeltaForBinaryEqualValues,
                            boost::none,
                            boost::none,
                            deltaInt32(elems[4].Obj()["y"_sd], elems[1].Obj()["y"_sd]),
                            boost::none},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {boost::none,
                            boost::none,
                            kDeltaForBinaryEqualValues,
                            boost::none,
                            boost::none,
                            deltaInt32(elems[5].Obj()["z"_sd], elems[2].Obj()["z"_sd])},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}, TestPath{{"y"}}, TestPath{{"z"}}};
    verifyDecompressPathFast(binData, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedAlternatingMergeRightLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                      createElementObj(BSON("y" << 2)),
                                      createElementObj(BSON("z" << 3)),
                                      createElementObj(BSON("x" << 2)),
                                      createElementObj(BSON("y" << 3)),
                                      createElementObj(BSON("z" << 4))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected,
                                 BSON("x" << elems[0].Obj().firstElement().Int() << "y"
                                          << elems[1].Obj().firstElement().Int() << "z"
                                          << elems[2].Obj().firstElement().Int()));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            boost::none,
                            boost::none,
                            deltaInt32(elems[3].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                            boost::none,
                            boost::none},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {boost::none,
                            kDeltaForBinaryEqualValues,
                            boost::none,
                            boost::none,
                            deltaInt32(elems[4].Obj()["y"_sd], elems[1].Obj()["y"_sd]),
                            boost::none},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {boost::none,
                            boost::none,
                            kDeltaForBinaryEqualValues,
                            boost::none,
                            boost::none,
                            deltaInt32(elems[5].Obj()["z"_sd], elems[2].Obj()["z"_sd])},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}, TestPath{{"y"}}, TestPath{{"z"}}};
    verifyDecompressPathFast(expected, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedAlternatingMergeLeftThenRight) {
    std::vector<BSONElement> elems = {createElementObj(BSON("z" << 1)),
                                      createElementObj(BSON("y" << 2 << "z" << 2)),
                                      createElementObj(BSON("x" << 3 << "z" << 3))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected,
                           BSON("y" << elems[1].Obj().firstElement().Int() << "x"
                                    << elems[2].Obj().firstElement().Int() << "z"
                                    << elems[0].Obj().firstElement().Int()));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {boost::none, kDeltaForBinaryEqualValues, boost::none}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {boost::none, boost::none, kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd]),
                            deltaInt32(elems[2].Obj()["z"_sd], elems[1].Obj()["z"_sd])},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}, TestPath{{"y"}}, TestPath{{"z"}}};
    verifyDecompressPathFast(binData, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedAlternatingMergeLeftThenRightLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementObj(BSON("z" << 1)),
                                      createElementObj(BSON("y" << 2 << "z" << 2)),
                                      createElementObj(BSON("x" << 3 << "z" << 3))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected,
                                 BSON("y" << elems[1].Obj().firstElement().Int() << "x"
                                          << elems[2].Obj().firstElement().Int() << "z"
                                          << elems[0].Obj().firstElement().Int()));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {boost::none, kDeltaForBinaryEqualValues, boost::none}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {boost::none, boost::none, kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd]),
                            deltaInt32(elems[2].Obj()["z"_sd], elems[1].Obj()["z"_sd])},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}, TestPath{{"y"}}, TestPath{{"z"}}};
    verifyDecompressPathFast(expected, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedMergeWithUnrelatedArray) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("a" << BSON_ARRAY(1 << 2) << "z" << 1)),
        createElementObj(BSON("a" << BSON_ARRAY(1 << 2) << "y" << 2 << "z" << 2)),
        createElementObj(BSON("a" << BSON_ARRAY(1 << 2) << "x" << 3 << "z" << 3))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected,
                           BSON("a" << BSON_ARRAY(1 << 2) << "y" << elems[1].Obj()["y"].Int() << "x"
                                    << elems[2].Obj()["x"].Int() << "z"
                                    << elems[0].Obj()["z"].Int()));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, kDeltaForBinaryEqualValues, kDeltaForBinaryEqualValues},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, kDeltaForBinaryEqualValues, kDeltaForBinaryEqualValues},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {boost::none, kDeltaForBinaryEqualValues, boost::none}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {boost::none, boost::none, kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd]),
                            deltaInt32(elems[2].Obj()["z"_sd], elems[1].Obj()["z"_sd])},
                           1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}, TestPath{{"y"}}, TestPath{{"z"}}};
    verifyDecompressPathFast(binData, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedMergeWithScalarObjectMismatch) {
    // Test that we can successfully build reference object when there are unrelated fields with
    // object and scalar mismatch.
    std::vector<BSONElement> elems = {createElementObj(BSON("z" << BSON("x" << 1))),
                                      createElementObj(BSON("y" << 1 << "z" << BSON("x" << 2)))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(
        expected, BSON("y" << elems[1].Obj()["y"_sd].Int() << "z" << elems[0].Obj()["z"_sd].Obj()));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {boost::none, kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaInt32(elems[1].Obj()["z"_sd].Obj()["x"_sd], elems[0].Obj()["z"_sd].Obj()["x"_sd])},
        1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    std::vector<TestPath> testPaths{TestPath{{"z", "x"}}, TestPath{{"y"}}};
    verifyDecompressPathFast(binData, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedMergeWithScalarObjectMismatchLegacyDecompress) {
    // Test that we can successfully build reference object when there are unrelated fields with
    // object and scalar mismatch.
    std::vector<BSONElement> elems = {createElementObj(BSON("z" << BSON("x" << 1))),
                                      createElementObj(BSON("y" << 1 << "z" << BSON("x" << 2)))};

    BufBuilder expected;
    appendInterleavedStartLegacy(
        expected, BSON("y" << elems[1].Obj()["y"_sd].Int() << "z" << elems[0].Obj()["z"_sd].Obj()));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {boost::none, kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaInt32(elems[1].Obj()["z"_sd].Obj()["x"_sd], elems[0].Obj()["z"_sd].Obj()["x"_sd])},
        1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
    std::vector<TestPath> testPaths{TestPath{{"z", "x"}}, TestPath{{"y"}}};
    verifyDecompressPathFast(expected, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedArrayAppend) {
    std::vector<BSONElement> elems = {
        createElementArray(BSONArrayBuilder().arr()),
        createElementArray(BSON_ARRAY(1)),
        createElementArray(BSON_ARRAY(1 << 2)),
        createElementArray(BSON_ARRAY(1 << 2 << 3)),
        createElementArray(BSON_ARRAY(1 << 2 << 3 << 4)),
    };

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendInterleavedStartArrayRoot(expected, elems[4].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {boost::none,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {boost::none, boost::none, kDeltaForBinaryEqualValues, kDeltaForBinaryEqualValues},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected, {boost::none, boost::none, boost::none, kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems, false /* testPathDecompression */);
}

TEST_F(BSONColumnTest, InterleavedIncompatibleMerge) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                      createElementObj(BSON("x" << 2 << "y" << 2)),
                                      createElementObj(BSON("y" << 3 << "x" << 3))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(
        expected,
        BSON("x" << elems[0].Obj().firstElement().Int() << "y" << elems[1].Obj()["y"_sd].Int()));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {boost::none, kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    verifyDecompressPathFast(binData, elems, TestPath{{"x"}});
}

TEST_F(BSONColumnTest, InterleavedIncompatibleMergeLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                      createElementObj(BSON("x" << 2 << "y" << 2)),
                                      createElementObj(BSON("y" << 3 << "x" << 3))};

    BufBuilder expected;
    appendInterleavedStartLegacy(
        expected,
        BSON("x" << elems[0].Obj().firstElement().Int() << "y" << elems[1].Obj()["y"_sd].Int()));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
        1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {boost::none, kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendInterleavedStartLegacy(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
    std::vector<TestPath> testPaths{TestPath{{"x"}}};
    verifyDecompressPathFast(expected, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedIncompatibleMergeMiddle) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("a" << 1 << "x" << 2 << "y" << 2 << "b" << 2)),
        createElementObj(BSON("a" << 1 << "y" << 3 << "x" << 3 << "b" << 2))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;

    appendInterleavedStart(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendInterleavedStart(expected, elems[1].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedIncompatibleMergeMiddleLegacyDecompress) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("a" << 1 << "x" << 2 << "y" << 2 << "b" << 2)),
        createElementObj(BSON("a" << 1 << "y" << 3 << "x" << 3 << "b" << 2))};

    BufBuilder expected;

    appendInterleavedStartLegacy(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendInterleavedStartLegacy(expected, elems[1].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedIncompatibleAfterDeterminedReference) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                      createElementObj(BSON("x" << 2)),
                                      createElementObj(BSON("x" << 3)),
                                      createElementObj(BSON("x" << 4)),
                                      createElementObj(BSON("x" << 5)),
                                      createElementObj(BSON("x" << 6)),
                                      createElementObj(BSON("x" << 0 << "y" << 0))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                            deltaInt32(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd]),
                            deltaInt32(elems[3].Obj()["x"_sd], elems[2].Obj()["x"_sd]),
                            deltaInt32(elems[4].Obj()["x"_sd], elems[3].Obj()["x"_sd]),
                            deltaInt32(elems[5].Obj()["x"_sd], elems[4].Obj()["x"_sd])},
                           1);
    appendEOO(expected);
    appendInterleavedStart(expected, elems[6].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedIncompatibleAfterDeterminedReferenceLegacyDecompress) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                      createElementObj(BSON("x" << 2)),
                                      createElementObj(BSON("x" << 3)),
                                      createElementObj(BSON("x" << 4)),
                                      createElementObj(BSON("x" << 5)),
                                      createElementObj(BSON("x" << 6)),
                                      createElementObj(BSON("x" << 0 << "y" << 0))};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                            deltaInt32(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd]),
                            deltaInt32(elems[3].Obj()["x"_sd], elems[2].Obj()["x"_sd]),
                            deltaInt32(elems[4].Obj()["x"_sd], elems[3].Obj()["x"_sd]),
                            deltaInt32(elems[5].Obj()["x"_sd], elems[4].Obj()["x"_sd])},
                           1);
    appendEOO(expected);
    appendInterleavedStartLegacy(expected, elems[6].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedSkipAfterEmptySubObj) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2 << "z" << BSON("z1" << BSONObjBuilder().obj()))),
        BSONElement()};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, boost::none);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedSkipAfterEmptySubObjLegacyDecompression) {
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2 << "z" << BSON("z1" << BSONObjBuilder().obj()))),
        BSONElement()};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, boost::none);
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, InterleavedSkipAfterEmptySubArray) {
    std::vector<BSONElement> elems = {
        createElementObj(
            BSON("x" << 1 << "y" << 2 << "z" << BSON("z1" << BSONArrayBuilder().arr()))),
        BSONElement()};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);

    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, boost::none);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, ObjectEmpty) {
    std::vector<BSONElement> elems = {createElementObj(BSONObjBuilder().obj()),
                                      createElementObj(BSONObjBuilder().obj())};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems, false /* testPathDecompression */);
}

TEST_F(BSONColumnTest, ObjectEmptyAfterNonEmpty) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                      createElementObj(BSONObjBuilder().obj())};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);
    appendLiteral(expected, elems[1]);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems, false /* testPathDecompression */);
}

TEST_F(BSONColumnTest, ObjectEmptyAfterNonEmptyLegacyDecompression) {
    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                      createElementObj(BSONObjBuilder().obj())};

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);
    appendLiteral(expected, elems[1]);
    appendEOO(expected);

    verifyDecompression(expected, elems, false /* testPathDecompression */);
}

TEST_F(BSONColumnTest, ObjectWithOnlyEmptyObjsDoesNotStartInterleaving) {
    std::vector<BSONElement> elems;
    elems.push_back(createElementObj(BSON("a" << BSONObjBuilder().obj())));
    elems.push_back(createElementObj(BSON("b" << BSONObjBuilder().obj())));

    for (const auto& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems[0]);
    appendLiteral(expected, elems[1]);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems, false /* testPathDecompression */);
}

TEST_F(BSONColumnTest, ObjectWithOnlyEmptyObjsDoesNotStartInterleavingFromDetermine) {
    // Append elements so we are in kSubObjDeterminingReference state when element with 'b'
    // field is appended. Make sure this does not re-start subobj compression as it only contain
    // empty subobj.
    std::vector<BSONElement> elems;
    elems.push_back(createElementObj(BSON("a" << 1)));
    elems.push_back(createElementObj(BSON("b" << BSONObjBuilder().obj())));

    for (const auto& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    appendLiteral(expected, elems[1]);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems, false /* testPathDecompression */);
}

TEST_F(BSONColumnTest,
       ObjectWithOnlyEmptyObjsDoesNotStartInterleavingFromDetermineLegacyDecompression) {
    // Append elements so we are in kSubObjDeterminingReference state when element with 'b'
    // field is appended. Make sure this does not re-start subobj compression as it only contain
    // empty subobj.
    std::vector<BSONElement> elems;
    elems.push_back(createElementObj(BSON("a" << 1)));
    elems.push_back(createElementObj(BSON("b" << BSONObjBuilder().obj())));


    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    appendLiteral(expected, elems[1]);
    appendEOO(expected);

    verifyDecompression(expected, elems, false /* testPathDecompression */);
}


TEST_F(BSONColumnTest, ObjectWithOnlyEmptyObjsDoesNotStartInterleavingFromAppending) {
    // Append enough elements so we are in kSubObjAppending state when element with 'b' field is
    // appended. Make sure this does not re-start subobj compression as it only contain empty
    // subobj.
    std::vector<BSONElement> elems;
    elems.push_back(createElementObj(BSON("a" << 1)));
    elems.push_back(createElementObj(BSON("a" << 2)));
    elems.push_back(createElementObj(BSON("a" << 3)));
    elems.push_back(createElementObj(BSON("a" << 4)));
    elems.push_back(createElementObj(BSON("a" << 5)));
    elems.push_back(createElementObj(BSON("a" << 6)));
    elems.push_back(createElementObj(BSON("b" << BSONObjBuilder().obj())));

    for (const auto& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["a"], elems[0].Obj()["a"]),
                            deltaInt32(elems[2].Obj()["a"], elems[1].Obj()["a"]),
                            deltaInt32(elems[3].Obj()["a"], elems[2].Obj()["a"]),
                            deltaInt32(elems[4].Obj()["a"], elems[3].Obj()["a"]),
                            deltaInt32(elems[5].Obj()["a"], elems[4].Obj()["a"])},
                           1);
    appendEOO(expected);

    appendLiteral(expected, elems[6]);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems, false /* testPathDecompression */);
}

TEST_F(BSONColumnTest,
       ObjectWithOnlyEmptyObjsDoesNotStartInterleavingFromAppendingLegacyDecompression) {
    // Append enough elements so we are in kSubObjAppending state when element with 'b' field is
    // appended. Make sure this does not re-start subobj compression as it only contain empty
    // subobj.
    std::vector<BSONElement> elems;
    elems.push_back(createElementObj(BSON("a" << 1)));
    elems.push_back(createElementObj(BSON("a" << 2)));
    elems.push_back(createElementObj(BSON("a" << 3)));
    elems.push_back(createElementObj(BSON("a" << 4)));
    elems.push_back(createElementObj(BSON("a" << 5)));
    elems.push_back(createElementObj(BSON("a" << 6)));
    elems.push_back(createElementObj(BSON("b" << BSONObjBuilder().obj())));

    BufBuilder expected;
    appendInterleavedStartLegacy(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["a"], elems[0].Obj()["a"]),
                            deltaInt32(elems[2].Obj()["a"], elems[1].Obj()["a"]),
                            deltaInt32(elems[3].Obj()["a"], elems[2].Obj()["a"]),
                            deltaInt32(elems[4].Obj()["a"], elems[3].Obj()["a"]),
                            deltaInt32(elems[5].Obj()["a"], elems[4].Obj()["a"])},
                           1);
    appendEOO(expected);

    appendLiteral(expected, elems[6]);
    appendEOO(expected);

    verifyDecompression(expected, elems, false /* testPathDecompression */);
}

TEST_F(BSONColumnTest, InterleavedFullSkipAfterObjectSkip) {
    // This test makes sure we're not leaking the skip from the 'yyyyyy' field into the next
    // measurement. 'yyyyyy' will be written into the buffer for the second item before we realize
    // that it only contain skips. We must not attempt to interpret this memory when the next
    // measurement is all skips.
    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "yyyyyy" << BSON("z" << 2))),
        createElementObj(BSON("x" << 1)),
        BSONElement()};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems.front().Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                            boost::none},
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues, boost::none, boost::none}, 1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    std::vector<TestPath> testPaths{TestPath{{"yyyyyy", "z"}}};
    verifyDecompressPathFast(binData, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedEmptySequence) {
    auto elem = createElementObj(BSON("x" << 1 << "y" << 2));

    BufBuilder interleavedBinary;
    appendInterleavedStart(interleavedBinary, elem.Obj());
    appendEOO(interleavedBinary);
    appendEOO(interleavedBinary);

    BSONColumnBlockBased colBlockBased{interleavedBinary.buf(),
                                       static_cast<size_t>(interleavedBinary.len())};
    boost::intrusive_ptr allocator{new BSONElementStorage()};
    std::vector<BSONElement> collection;
    std::vector<std::pair<TestPath, std::vector<BSONElement>&>> testPaths{
        {TestPath{{"x"}}, collection}};
    ASSERT_THROWS_CODE(colBlockBased.decompress<BSONElementMaterializer>(collection, allocator),
                       DBException,
                       8625732);
    ASSERT_THROWS_CODE(
        colBlockBased.decompress<BSONElementMaterializer>(allocator, std::span(testPaths)),
        DBException,
        8625730);

    BSONColumn col(createBSONColumn(interleavedBinary.buf(), interleavedBinary.len()));
    ASSERT_THROWS_CODE(std::distance(col.begin(), col.end()), DBException, 9232700);
}


TEST_F(BSONColumnTest, NonZeroRLEInFirstBlockAfterSimple8bBlocks) {
    int64_t value = 1;

    // Start with values that give large deltas so we write out 16 simple8b blocks and end with a
    // non zero value that is equal to the deltas that will follow
    std::vector<BSONElement> elems = {createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFF),
                                      createElementInt64(0),      createElementInt64(value++),
                                      createElementInt64(value++)};

    // Add 120 additional elements that all get a delta of 1, because the last block ended with the
    // / same value they can be encoded with RLE.
    for (int i = 0; i < 120; ++i) {
        elems.push_back(createElementInt64(value++));
    }

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b1111);

    auto deltas = deltaInt64(elems.begin() + 1, elems.end(), elems.front());
    int blockCount = 0;
    auto writeFn = [&](uint64_t block) {
        if (blockCount++ == 16) {
            appendSimple8bControl(expected, 0b1000, 0b0000);
        }
        expected.appendNum(block);
        return true;
    };
    Simple8bBuilder<uint64_t> s8bBuilder;

    for (auto delta : deltas) {
        s8bBuilder.append(*delta, writeFn);
    }
    s8bBuilder.flush(writeFn);
    appendEOO(expected);

    // We should now have 16 regular Simple8b blocks and then a 17th using RLE at the end.
    ASSERT_EQ(blockCount, 17);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, NonZeroRLEInLastBlock) {
    // This test creates a BSONColumn control block using the full 16 simple8b blocks where the last
    // block is RLE containing a non-zero value. Verify that we can handle that correctly,
    // especially when instantiating a BSONColumnBuilder from an already compressed binary, in that
    // case we need to put the values in the RLE block back to pending.
    int64_t value = 1;

    // Start with values that give large deltas so we write out 15 simple8b blocks and end with a
    // non zero value that is equal to the deltas that will follow
    std::vector<BSONElement> elems = {createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFFFFFFFF),
                                      createElementInt64(0),      createElementInt64(0xFF),
                                      createElementInt64(0),      createElementInt64(0xFF),
                                      createElementInt64(0),      createElementInt64(value++),
                                      createElementInt64(value++)};

    // Add 120 additional elements that all get a delta of 1, because the last block ended with the
    // same value they can be encoded with RLE.
    for (int i = 0; i < 120; ++i) {
        elems.push_back(createElementInt64(value++));
    }

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b1111);

    auto deltas = deltaInt64(elems.begin() + 1, elems.end(), elems.front());
    int blockCount = 0;
    auto writeFn = [&](uint64_t block) {
        expected.appendNum(block);
        ++blockCount;
        return true;
    };
    Simple8bBuilder<uint64_t> s8bBuilder;

    for (auto delta : deltas) {
        s8bBuilder.append(*delta, writeFn);
    }

    // Verify that we have not yet written the last RLE block. This will happen during flush
    // (equivalent to BSONColumn::finalize).
    ASSERT_EQ(blockCount, 15);

    s8bBuilder.flush(writeFn);
    appendEOO(expected);

    // We should now have 15 regular Simple8b blocks and then a 16th using RLE at the end.
    ASSERT_EQ(blockCount, 16);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, ZeroDeltaAfterInterleaved) {
    auto obj = createElementObj(BSON("a" << 1));
    std::vector<BSONElement> elems = {obj, obj};

    BufBuilder expected;
    appendInterleavedStart(expected, obj.Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    verifyDecompression(expected, elems);
    std::vector<TestPath> testPaths{TestPath{{"a"}}};
    verifyDecompressPathFast(expected, elems, testPaths);
}

TEST_F(BSONColumnTest, InvalidControlByte) {
    auto elem = createElementInt32(0);

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendSimple8bControl(expected, 0b0010, 0b0000);
    appendSimple8bBlock64(expected, deltaInt32(elem, elem));
    appendEOO(expected);

    try {
        BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
        for (auto it = col.begin(), e = col.end(); it != e; ++it) {
        }
        ASSERT(false);

    } catch (DBException&) {
    }
}

TEST_F(BSONColumnTest, InvalidSize) {
    auto elem = createElementInt32(0);

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaInt32(elem, elem));
    appendEOO(expected);

    try {
        BSONColumn col(createBSONColumn(expected.buf(), expected.len() - 2));
        for (auto it = col.begin(), e = col.end(); it != e; ++it) {
        }
        ASSERT(false);

    } catch (DBException&) {
    }
}

TEST_F(BSONColumnTest, InvalidDoubleScale) {
    auto d1 = createElementDouble(1.11);
    auto d2 = createElementDouble(1.12);

    BufBuilder expected;
    appendLiteral(expected, d1);
    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bBlock64(expected, deltaDouble(d2, d1, 100));
    appendEOO(expected);

    try {
        BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
        for (auto it = col.begin(), e = col.end(); it != e; ++it) {
        }
        ASSERT(false);

    } catch (DBException&) {
    }
}

TEST_F(BSONColumnTest, MissingEOO) {
    auto elem = createElementInt32(0);

    BufBuilder expected;
    appendLiteral(expected, elem);

    try {
        BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
        for (auto it = col.begin(), e = col.end(); it != e; ++it) {
        }
        ASSERT(false);

    } catch (DBException&) {
    }
}

TEST_F(BSONColumnTest, EmptyBuffer) {
    BufBuilder expected;

    try {
        BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
        for (auto it = col.begin(), e = col.end(); it != e; ++it) {
        }
        ASSERT(false);

    } catch (DBException&) {
    }
}

TEST_F(BSONColumnTest, InvalidSimple8b) {
    // A Simple8b block with an invalid selector throws an error when iterating.
    std::vector<uint8_t> invalidSelectors = {0, 0xA7, 0xB7, 0xC7, 0xD7, 0xE7, 0xF7, 0xE8, 0xF8};

    for (auto&& selector : invalidSelectors) {
        auto elem = createElementInt32(0);
        BufBuilder expected;
        appendLiteral(expected, elem);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        expected.appendNum(static_cast<uint64_t>(selector));
        appendEOO(expected);

        BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
        ASSERT_THROWS(std::distance(col.begin(), col.end()), DBException);
    }
}

TEST_F(BSONColumnTest, InvalidInterleavedCount) {
    // This test sets up an interleaved reference object with two fields but only provides one
    // interleaved substream.
    auto test = [&](auto appendInterleavedStartFunc) {
        BufBuilder expected;
        appendInterleavedStartFunc(expected, BSON("a" << 1 << "b" << 1));
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);
        appendEOO(expected);

        BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
        ASSERT_THROWS(std::distance(col.begin(), col.end()), DBException);
    };

    test(appendInterleavedStartLegacy);
    test(appendInterleavedStart);

    BufBuilder expected;
    appendInterleavedStartArrayRoot(expected, BSON_ARRAY(1 << 1));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendEOO(expected);

    BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
    ASSERT_THROWS(std::distance(col.begin(), col.end()), DBException);
}

TEST_F(BSONColumnTest, InvalidInterleavedWhenAlreadyInterleaved) {
    // This tests that we handle the interleaved start byte when already in interleaved mode.
    auto test = [&](auto appendInterleavedStartFunc) {
        BufBuilder expected;
        appendInterleavedStartFunc(expected, BSON("a" << 1 << "b" << 1));
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendInterleavedStartFunc(expected, BSON("a" << 1 << "b" << 1));
        appendEOO(expected);
        appendEOO(expected);

        BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
        ASSERT_THROWS(std::distance(col.begin(), col.end()), DBException);
    };

    test(appendInterleavedStartLegacy);
    test(appendInterleavedStart);

    BufBuilder expected;
    appendInterleavedStart(expected, BSON("a" << 1 << "b" << 1));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendInterleavedStartArrayRoot(expected, BSON_ARRAY("a" << 1 << "b" << 1));
    appendEOO(expected);
    appendEOO(expected);

    BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
    ASSERT_THROWS(std::distance(col.begin(), col.end()), DBException);
}

TEST_F(BSONColumnTest, InvalidDeltaAfterInterleaved) {
    // This test uses a non-zero delta value after an interleaved object, which is invalid.
    auto test = [&](auto appendInterleavedStartFunc) {
        BufBuilder expected;
        appendInterleavedStartFunc(expected, BSON("a" << 1));
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlock64(expected, Simple8bTypeUtil::encodeInt64(1));
        appendEOO(expected);

        BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
        ASSERT_THROWS_CODE(std::distance(col.begin(), col.end()), DBException, 6785500);
    };

    test(appendInterleavedStartLegacy);
    test(appendInterleavedStart);

    BufBuilder expected;
    appendInterleavedStartArrayRoot(expected, BSON_ARRAY(1));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, Simple8bTypeUtil::encodeInt64(1));
    appendEOO(expected);

    BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
    ASSERT_THROWS_CODE(std::distance(col.begin(), col.end()), DBException, 6785500);
}

TEST_F(BSONColumnTest, InvalidDelta) {
    testInvalidDelta(createRegex());
    testInvalidDelta(createDBRef("ns", OID{"112233445566778899AABBCC"}));
    testInvalidDelta(createCodeWScope("code", BSONObj{}));
    testInvalidDelta(createSymbol("symbol"));
    testInvalidDelta(BSON("obj" << BSON("a" << 1)).firstElement());
    testInvalidDelta(BSON("arr" << BSON_ARRAY("a")).firstElement());
}

TEST_F(BSONColumnTest, AppendMinKey) {
    cb.append(createElementMinKey());

    BufBuilder expected;
    appendLiteral(expected, createElementMinKey());
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {createElementMinKey()});
}

TEST_F(BSONColumnTest, AppendMaxKey) {
    cb.append(createElementMaxKey());

    BufBuilder expected;
    appendLiteral(expected, createElementMaxKey());
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {createElementMaxKey()});
}

TEST_F(BSONColumnTest, AppendMinKeyInSubObj) {
    BSONObjBuilder obj;
    {
        BSONObjBuilder builder = obj.subobjStart("root");
        builder.append(createElementMinKey());
    }

    BSONObj ref = obj.obj();
    cb.append(createElementObj(ref));

    BufBuilder expected;
    appendInterleavedStart(expected, ref);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {createElementObj(ref)});
}

TEST_F(BSONColumnTest, AppendMaxKeyInSubObj) {
    BSONObjBuilder obj;
    {
        BSONObjBuilder builder = obj.subobjStart("root");
        builder.append(createElementMaxKey());
    }

    BSONObj ref = obj.obj();
    cb.append(createElementObj(ref));

    BufBuilder expected;
    appendInterleavedStart(expected, ref);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {createElementObj(ref)});
}

TEST_F(BSONColumnTest, AppendMinKeyInSubObjAfterInterleaveStart) {
    BSONObjBuilder obj;
    {
        BSONObjBuilder builder = obj.subobjStart("root");
        builder.append(createElementMinKey());
    }

    std::vector<BSONElement> elems = {createElementObj(BSON("root" << BSON("0" << 1))),
                                      createElementObj(obj.obj())};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendLiteral(expected, createElementMinKey());
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, AppendMinKeyInSubObjAfterInterleaveStartInAppendMode) {
    BSONObjBuilder obj;
    {
        BSONObjBuilder builder = obj.subobjStart("root");
        builder.append(createElementMinKey());
    }

    std::vector<BSONElement> elems(7, createElementObj(BSON("root" << BSON("0" << 1))));
    elems.push_back(createElementObj(obj.obj()));

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues},
                           1);
    appendLiteral(expected, createElementMinKey());
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, AppendMinKeyInSubObjAfterMerge) {
    BSONObjBuilder obj;
    {
        BSONObjBuilder builder = obj.subobjStart("root");
        builder.append("a", "asd");
        builder.append(createElementMinKey());
    }

    // Make sure we handle MinKey even if we would detect that "a" needs to be merged before
    // observing the MinKey.
    std::vector<BSONElement> elems = {createElementObj(BSON("root" << BSON("0" << 1))),
                                      createElementObj(obj.obj())};

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendInterleavedStart(expected,
                           BSON("root" << BSON("a" << "asd"
                                                   << "0" << 1)));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {
                               boost::none,
                               kDeltaForBinaryEqualValues,
                           },
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {
                               kDeltaForBinaryEqualValues,
                           },
                           1);
    appendLiteral(expected, createElementMinKey());
    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, DecompressMinKey) {
    BufBuilder expected;
    appendLiteral(expected, createElementMinKey());
    appendEOO(expected);

    verifyDecompression(expected, {createElementMinKey()});
}

TEST_F(BSONColumnTest, DecompressMaxKey) {
    BufBuilder expected;
    appendLiteral(expected, createElementMaxKey());
    appendEOO(expected);

    verifyDecompression(expected, {createElementMaxKey()});
}

TEST_F(BSONColumnTest, DecompressMinKeyInSubObj) {
    BSONObjBuilder obj;
    {
        BSONObjBuilder builder = obj.subobjStart("root");
        builder.append(createElementMinKey());
    }

    BSONObj ref = obj.obj();

    BufBuilder expected;
    appendInterleavedStart(expected, ref);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, {createElementObj(ref)});
}

TEST_F(BSONColumnTest, DecompressMinKeyInSubObjAfterInterleaveStart) {
    BSONObjBuilder obj;
    {
        BSONObjBuilder builder = obj.subobjStart("root");
        builder.append(createElementMinKey());
    }

    std::vector<BSONElement> elems = {createElementObj(BSON("root" << BSON("0" << 1))),
                                      createElementObj(obj.obj())};

    BufBuilder expected;
    appendInterleavedStart(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
    appendLiteral(expected, createElementMinKey());
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, DecompressMinKeyInSubObjAfterInterleaveStartInAppendMode) {
    BSONObjBuilder obj;
    {
        BSONObjBuilder builder = obj.subobjStart("root");
        builder.append(createElementMinKey());
    }

    std::vector<BSONElement> elems(7, createElementObj(BSON("root" << BSON("0" << 1))));
    elems.push_back(createElementObj(obj.obj()));

    BufBuilder expected;
    appendInterleavedStart(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues},
                           1);
    appendLiteral(expected, createElementMinKey());
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, DecompressMinKeyInSubObjAfterMerge) {
    BSONObjBuilder obj;
    {
        BSONObjBuilder builder = obj.subobjStart("root");
        builder.append("a", "asd");
        builder.append(createElementMinKey());
    }

    // Make sure we handle MinKey even if we would detect that "a" needs to be merged before
    // observing the MinKey.
    std::vector<BSONElement> elems = {createElementObj(BSON("root" << BSON("0" << 1))),
                                      createElementObj(obj.obj())};

    BufBuilder expected;
    appendInterleavedStart(expected,
                           BSON("root" << BSON("a" << "asd"
                                                   << "0" << 1)));
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {
                               boost::none,
                               kDeltaForBinaryEqualValues,
                           },
                           1);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(expected,
                           {
                               kDeltaForBinaryEqualValues,
                           },
                           1);
    appendLiteral(expected, createElementMinKey());
    appendEOO(expected);
    appendEOO(expected);

    verifyDecompression(expected, elems);
}

TEST_F(BSONColumnTest, AppendObjDirectly) {
    BSONColumnBuilder cb2;

    std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                      createElementObj(BSON("x" << 2))};

    for (auto&& elem : elems) {
        cb.append(elem);
        cb2.append(elem.Obj());
    }

    auto binData = cb.finalize();
    auto binData2 = cb2.finalize();
    ASSERT_EQ(binData.length, binData2.length);
    ASSERT_EQ(memcmp(binData.data, binData2.data, binData.length), 0);
}

TEST_F(BSONColumnTest, AppendArrayDirectly) {
    BSONColumnBuilder cb2;

    std::vector<BSONElement> elems = {createElementArray(BSON_ARRAY(1 << 2 << 3)),
                                      createElementArray(BSON_ARRAY(2 << 4 << 6))};

    for (auto&& elem : elems) {
        cb.append(elem);
        cb2.append(BSONArray(elem.Obj()));
    }

    auto binData = cb.finalize();
    auto binData2 = cb2.finalize();
    ASSERT_EQ(binData.length, binData2.length);
    ASSERT_EQ(memcmp(binData.data, binData2.data, binData.length), 0);
}

TEST_F(BSONColumnTest, Intermediate) {
    // Verify that the intermediate function works as expected
    BSONColumnBuilder reference;

    // Various elements
    std::vector<BSONElement> elems = {createElementDouble(1.0),
                                      createElementDouble(2.0),
                                      BSONElement(),
                                      createElementDouble(2.1),
                                      createElementDouble(2.2),
                                      createElementDouble(1.1),
                                      createElementDouble(2.1),
                                      BSONElement(),
                                      createElementDouble(2.2),
                                      createElementDouble(2.3),
                                      createElementDouble(3.12345678),
                                      createElementDouble(1.12345671),
                                      createElementDouble(1.12345672),
                                      createElementDouble(2),
                                      createElementDouble(3),
                                      createElementDouble(94.8),
                                      createElementDouble(107.9),
                                      createElementDouble(111.9),
                                      createElementDouble(113.4),
                                      createElementDouble(89.0),
                                      createElementDouble(126.7),
                                      createElementDouble(119.0),
                                      createElementDouble(105.0),
                                      createElementDouble(120.0),
                                      createElementDouble(1.12345671),
                                      createElementDouble(2),
                                      BSONElement(),
                                      BSONElement(),
                                      createElementDouble(3),
                                      createElementDouble(1.12345671),
                                      createElementDouble(1.12345672),
                                      createElementDouble(2),
                                      createElementDouble(3),
                                      createElementDouble(1.12345672),
                                      createElementDouble(1.12345671),
                                      createElementDouble(2),
                                      BSONElement(),
                                      BSONElement(),
                                      createElementDouble(1.12345671),
                                      createElementDouble(116.0),
                                      createElementDouble(95.0),
                                      createElementDouble(80.0),
                                      createElementDouble(87.0),
                                      createElementDouble(113.0),
                                      createElementDouble(90.0),
                                      createElementDouble(113.0),
                                      createElementDouble(93.0),
                                      createElementDouble(99.0),
                                      createElementDouble(123.0),
                                      createElementDouble(89.0),
                                      createElementDouble(92.0)};


    {
        auto diff = cb.intermediate();
        ASSERT_EQ(diff.size(), 1);
        ASSERT_EQ(*diff.data(), '\0');
        ASSERT_EQ(diff.offset(), 0);
    }

    BufBuilder buffer;
    // Vector of reopen builders, we will reopen all intermediate binaries and continue appending to
    // make sure they all produce the same binaries in the end.
    std::vector<std::pair<BSONColumnBuilder<>, BufBuilder>> reopenBuilders;
    for (auto&& elem : elems) {
        // Append element to all builders, inclusive our reopenBuilders
        cb.append(elem);
        reference.append(elem);

        auto diff = cb.intermediate();
        ASSERT_GTE(buffer.len(), diff.offset());
        buffer.setlen(diff.offset());
        buffer.appendBuf(diff.data(), diff.size());

        // Perform the same intermediate on the builders that was reopened
        for (auto&& reopen : reopenBuilders) {
            reopen.first.append(elem);
            auto d = reopen.first.intermediate();
            reopen.second.setlen(d.offset());
            reopen.second.appendBuf(d.data(), d.size());
        }

        // Reopen this binary
        BufBuilder reopenBuf;
        reopenBuf.appendBuf(buffer.buf(), buffer.len());
        reopenBuilders.push_back(
            std::make_pair(BSONColumnBuilder<>(buffer.buf(), buffer.len()), std::move(reopenBuf)));
    }

    // Verify that the binaries are exactly compared to a builder using finalize
    auto binData = reference.finalize();

    assertBinaryEqual(binData, buffer);
    for (auto&& reopen : reopenBuilders) {
        assertBinaryEqual(binData, reopen.second);
    }
}

TEST_F(BSONColumnTest, DecompressPathFastLargeDeltaIsLiteralAfterSimple8b) {
    BSONColumnBuilder cb;

    std::vector<BSONElement> values = {createElementInt64(0),
                                       createElementInt64(0),
                                       createElementInt64(std::numeric_limits<int64_t>::max()),
                                       createElementInt64(std::numeric_limits<int64_t>::max())};

    std::vector<BSONElement> elems;

    for (auto val : values) {
        auto elem = createElementObj(BSON("a" << val));
        elems.push_back(elem);
        cb.append(elem);
    }

    BufBuilder expected;

    appendInterleavedStart(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);

    // The two deltas for the 0s.
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues, kDeltaForBinaryEqualValues}, 1);

    // large is too large, so we need an uncompressed literal and a new control and delta blocks
    auto large = values[2];
    appendLiteral(expected, large);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaInt64(large, large));

    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);

    TestPath testPath{{"a"}};
    verifyDecompressPathFast(binData, elems, testPath);
}

TEST_F(BSONColumnTest, DecompressPathFastOIDLargeDeltaIsLiteralAfterSimple8b) {
    BSONColumnBuilder cb;

    std::vector<BSONElement> values = {createObjectId(OID("112233445566778899AABBCC")),
                                       createObjectId(OID("112233445566778899AABBCC")),
                                       createObjectId(OID::max()),
                                       createObjectId(OID::max())};

    std::vector<BSONElement> elems;

    for (auto val : values) {
        auto elem = createElementObj(BSON("a" << val));
        elems.push_back(elem);
        cb.append(elem);
    }

    BufBuilder expected;

    appendInterleavedStart(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);

    // The two deltas for the OID("A").
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaOfDeltaObjectId(values[1], values[0], values[0])},
        1);

    // large is too large, so we need an uncompressed literal and a new control and delta blocks
    auto large = values[2];
    appendLiteral(expected, large);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaOfDeltaObjectId(values[3], large, large));

    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);

    TestPath testPath{{"a"}};
    verifyDecompressPathFast(binData, elems, testPath);
}

TEST_F(BSONColumnTest, DecompressPathFastInterleavedIntsAndDoubles) {
    // Tests that decompressFast works when alternating types.
    BSONColumnBuilder cb;

    std::vector<BSONElement> values = {createElementInt32(0),
                                       createElementInt32(1),
                                       createElementDouble(2.0),
                                       createElementDouble(3.0),
                                       createElementInt32(4)};

    std::vector<BSONElement> elems;

    for (auto val : values) {
        auto elem = createElementObj(BSON("a" << val));
        elems.push_back(elem);
        cb.append(elem);
    }

    BufBuilder expected;

    appendInterleavedStart(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues, deltaInt32(elems[1].Obj()["a"_sd], elems[0].Obj()["a"_sd])},
        1);

    // Uncompressed literal since we are switching to doubles.
    appendLiteral(expected, values[2]);
    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bBlocks64(
        expected, {deltaDouble(elems[3].Obj()["a"_sd], elems[2].Obj()["a"_sd], 1)}, 1);

    // Uncompressed literal since we are switching back to ints.
    appendLiteral(expected, values[4]);

    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);

    TestPath testPath{{"a"}};
    verifyDecompressPathFast(binData, elems, testPath);
}

TEST_F(BSONColumnTest, DecompressPathFastInterleavedDatesAndDecimals) {
    // Tests that decompressFast works properly when interleaving dates which are delta-of-delta
    // types, and decimals which are 128 types.
    BSONColumnBuilder cb;

    std::vector<BSONElement> values = {createDate(Date_t::fromMillisSinceEpoch(1)),
                                       createDate(Date_t::fromMillisSinceEpoch(2)),
                                       createElementDecimal128(Decimal128(1)),
                                       createElementDecimal128(Decimal128(5)),
                                       createDate(Date_t::fromMillisSinceEpoch(8))};

    std::vector<BSONElement> elems;

    for (auto val : values) {
        auto elem = createElementObj(BSON("a" << val));
        elems.push_back(elem);
        cb.append(elem);
    }

    BufBuilder expected;

    appendInterleavedStart(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks64(
        expected,
        {kDeltaForBinaryEqualValues,
         deltaOfDeltaDate(elems[1].Obj()["a"_sd], elems[0].Obj()["a"_sd], elems[0].Obj()["a"_sd])},
        1);

    // Uncompressed literal since we are switching from dates to decimals.
    appendLiteral(expected, values[2]);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock128(expected,
                           {deltaDecimal128(elems[3].Obj()["a"_sd], elems[2].Obj()["a"_sd])});

    // Uncompressed literal when switching back from decimals to dates.
    appendLiteral(expected, values[4]);

    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);

    TestPath testPath{{"a"}};
    verifyDecompressPathFast(binData, elems, testPath);
}

TEST_F(BSONColumnTest, DecompressPathFastInterleavedStringsAndOIDs) {
    // Tests that decompressFast works properly when interleaving dates which are delta-of-delta
    // types, and decimals which are 128 types.
    BSONColumnBuilder cb;

    std::vector<BSONElement> values = {createElementString("hello_world0"),
                                       createElementString("hello_world1"),
                                       createObjectId(OID("112233445566778899AABBCC")),
                                       createObjectId(OID("112233445566778899AABBCB")),
                                       createElementString("hello_world3")};

    std::vector<BSONElement> elems;

    for (auto val : values) {
        auto elem = createElementObj(BSON("a" << val));
        elems.push_back(elem);
        cb.append(elem);
    }

    BufBuilder expected;

    appendInterleavedStart(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlocks128(
        expected, {kDeltaForBinaryEqualValues128, deltaString(values[1], values[0])}, 1);

    // Uncompressed literal since we are switching from Strings to OIDs.
    appendLiteral(expected, values[2]);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaOfDeltaObjectId(values[3], values[2], values[2]));

    // Uncompressed literal when switching back from OIDs to Strings.
    appendLiteral(expected, values[4]);

    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);

    TestPath testPath{{"a"}};
    verifyDecompressPathFast(binData, elems, testPath);
}

TEST_F(BSONColumnTest, DecompressPathFastNestedScalarsLargeDeltas) {
    BSONColumnBuilder cb;

    std::vector<BSONElement> values = {createElementInt64(0),
                                       createElementInt64(0),
                                       createElementInt64(std::numeric_limits<int64_t>::max()),
                                       createElementInt64(std::numeric_limits<int64_t>::max())};

    std::vector<BSONElement> elems;

    for (auto val : values) {
        auto elem = createElementObj(BSON("a" << BSON("b" << BSON("c" << val))));
        elems.push_back(elem);
        cb.append(elem);
    }

    BufBuilder expected;

    appendInterleavedStart(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b0000);

    // The two deltas for the 0s.
    appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues, kDeltaForBinaryEqualValues}, 1);

    // large is too large, so we need an uncompressed literal and a new control and delta blocks
    auto large = values[2];
    appendLiteral(expected, large);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, deltaInt64(large, large));

    appendEOO(expected);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);

    TestPath testPath{{"a", "b", "c"}};
    verifyDecompressPathFast(binData, elems, testPath);
}

TEST_F(BSONColumnTest, DecompressPathFastDuplicatePaths) {
    BSONColumnBuilder cb;

    std::vector<BSONElement> values = {
        createElementInt64(0),
        createElementInt64(10),
        createElementInt64(20),
        createElementInt64(30),
        createElementInt64(40),
        createElementInt64(50),
        createElementInt64(60),
        createElementInt64(70),
    };
    std::vector<BSONElement> elems;

    for (size_t i = 0; i < values.size(); i += 2) {
        auto elem = createElementObj(BSON("a" << values[i] << "b" << values[i + 1]));
        elems.push_back(elem);
        cb.append(elem);
    }

    auto binData = cb.finalize();
    verifyDecompression(binData, elems);

    // Test that we can decompress duplicate paths that refer to the same element.
    std::vector<TestPath> testPaths{TestPath{{"b"}}, TestPath{{"b"}}};
    verifyDecompressPathFast(binData, elems, testPaths);
}

TEST_F(BSONColumnTest, InterleavedAfterSkip) {
    std::vector<BSONElement> elems{
        BSONElement{},
        BSONElement{},
        createElementObj(BSON("a" << int32_t(100))),
        createElementObj(BSON("a" << int32_t(200))),
    };

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    auto binData = cb.finalize();

    BufBuilder expected;
    appendSimple8bControl(expected, 0b1000, 0b000);
    std::vector<boost::optional<uint64_t>> missingDeltas{boost::none, boost::none};
    appendSimple8bBlocks64(expected, missingDeltas, 1);
    appendInterleavedStart(expected, elems[2].Obj());
    appendSimple8bControl(expected, 0b1000, 0b000);
    std::vector<boost::optional<uint64_t>> aDeltas{
        deltaInt32(elems[2].Obj()["a"], elems[2].Obj()["a"]),
        deltaInt32(elems[3].Obj()["a"], elems[2].Obj()["a"])};
    appendSimple8bBlocks64(expected, aDeltas, 1);
    appendEOO(expected);  // End interleaved
    appendEOO(expected);  // End BSONColumn

    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    verifyDecompressPathFast(binData, elems, TestPath{{"a"}});
}

TEST_F(BSONColumnTest, SkipAfterInterleaved) {
    // These objects contain an empty subobject, so top-level skips cannot be represented as skips
    // in the scalar streams. The compressor will append the skips outside of interleaved mode.
    std::vector<BSONElement> elems{
        createElementObj(BSON("a" << int32_t(100) << "b" << BSONObj{})),
        createElementObj(BSON("a" << int32_t(200) << "b" << BSONObj{})),
        BSONElement{},
        BSONElement{},
        createElementObj(BSON("a" << int32_t(300) << "b" << BSONObj{})),
        createElementObj(BSON("a" << int32_t(400) << "b" << BSONObj{})),
        BSONElement{},
        BSONElement{},
    };

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    auto binData = cb.finalize();

    BufBuilder expected;
    appendInterleavedStart(expected, elems[0].Obj());
    appendSimple8bControl(expected, 0b1000, 0b000);
    std::vector<boost::optional<uint64_t>> aDeltas0{
        deltaInt32(elems[0].Obj()["a"], elems[0].Obj()["a"]),
        deltaInt32(elems[1].Obj()["a"], elems[0].Obj()["a"])};
    appendSimple8bBlocks64(expected, aDeltas0, 1);
    appendEOO(expected);  // End interleaved
    appendSimple8bControl(expected, 0b1000, 0b000);
    std::vector<boost::optional<uint64_t>> missingDeltas{boost::none, boost::none};
    appendSimple8bBlocks64(expected, missingDeltas, 1);

    appendInterleavedStart(expected, elems[4].Obj());
    appendSimple8bControl(expected, 0b1000, 0b000);
    std::vector<boost::optional<uint64_t>> aDeltas1{
        deltaInt32(elems[4].Obj()["a"], elems[4].Obj()["a"]),
        deltaInt32(elems[5].Obj()["a"], elems[4].Obj()["a"])};
    appendSimple8bBlocks64(expected, aDeltas1, 1);
    appendEOO(expected);  // End interleaved
    appendSimple8bControl(expected, 0b1000, 0b000);
    appendSimple8bBlocks64(expected, missingDeltas, 1);

    appendEOO(expected);  // End BSONColumn

    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
    verifyDecompressPathFast(binData, elems, TestPath{{"a"}});
}

TEST_F(BSONColumnTest, EmptyObjectSkipAndInterleaved) {
    // These objects contain an empty subobject, so top-level skips cannot be represented as skips
    // in the scalar streams. The compressor will append the skips outside of interleaved mode.
    std::vector<BSONElement> elems{
        BSONElement{},
        BSONElement{},
        createElementObj(BSONObj{}),
        createElementObj(BSONObj{}),
        createElementObj(BSONObj{}),
        BSONElement{},
        BSONElement{},
        createElementObj(BSON("a" << int32_t(100) << "b" << BSONObj{})),
        createElementObj(BSON("a" << int32_t(200) << "b" << BSONObj{})),
        BSONElement{},
        BSONElement{},
        createElementObj(BSON("a" << int32_t(300) << "b" << BSONObj{})),
        createElementObj(BSON("a" << int32_t(400) << "b" << BSONObj{})),
        BSONElement{},
        createElementObj(BSONObj{}),
        createElementObj(BSONObj{}),
        BSONElement{},
    };

    for (auto&& elem : elems) {
        cb.append(elem);
    }

    auto binData = cb.finalize();

    verifyDecompression(binData, elems, false /* testPathDecompression */);
    verifyDecompressPathFast(binData, elems, TestPath{{"a"}});
    verifyDecompressPathFast(binData, elems, TestPath{{"b"}});
    verifyDecompressPathFast(binData, elems, TestPath{{}});
}

TEST_F(BSONColumnTest, LegacyInterleavedPaths) {
    // This test encodes a BSONColumn with legacy interleaved mode. That means that arrays are
    // considered a leaf when traversing the reference object. Therefore we can't decompress
    // individual elements of any arrays. We need to return an error for paths that try to do this.
    std::vector<BSONElement> elems = {createElementObj(BSON("a" << BSON_ARRAY(10) << "b" << 20)),
                                      createElementObj(BSON("a" << BSON_ARRAY(10) << "b" << 20)),
                                      createElementObj(BSON("a" << BSON_ARRAY(10) << "b" << 20)),
                                      createElementObj(BSON("a" << BSON_ARRAY(10) << "b" << 20))};
    BufBuilder bb;
    appendInterleavedStartLegacy(bb, elems.front().Obj());
    appendSimple8bControl(bb, 0b1000, 0b0000);
    appendSimple8bBlocks64(bb,
                           {kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues},
                           1);
    appendSimple8bControl(bb, 0b1000, 0b0000);
    appendSimple8bBlocks64(bb,
                           {kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues,
                            kDeltaForBinaryEqualValues},
                           1);
    appendEOO(bb);
    appendEOO(bb);
    verifyDecompression(bb, elems);

    // Now try path-based decompression with a path that we cannot support.
    BSONColumnBlockBased column{bb.buf(), static_cast<size_t>(bb.len())};

    // This path is trying to get the element in the array. We cannot handle this path.
    struct ArrayElemPath {
        std::vector<const char*> elementsToMaterialize(BSONObj refObj) {
            return {refObj["a"].Array()[0].value()};
        }
    };

    std::vector<BSONElement> output;
    std::vector<std::pair<ArrayElemPath, std::vector<BSONElement>&>> paths = {
        {ArrayElemPath{}, output}};
    boost::intrusive_ptr allocator{new BSONElementStorage()};
    // 'Request for unknown element'
    ASSERT_THROWS_CODE(column.decompress<BSONElementMaterializer>(allocator, std::span(paths)),
                       DBException,
                       9071200);

    // Try again with a path that we can support.
    for (auto&& elem : elems) {
        cb.append(elem);
    }

    auto binData = cb.finalize();
    verifyDecompressPathFast(binData, elems, TestPath{{"b"}});
}

// The large literal emits this on Visual Studio: Fatal error C1091: compiler limit: string exceeds
// 65535 bytes in length
#if !defined(_MSC_VER) || _MSC_VER >= 1929
TEST_F(BSONColumnTest, FTDCRoundTrip) {
    StringData compressedBase64Encoded = {
#include "mongo/bson/column/bson_column_compressed_data.inl"  // IWYU pragma: keep
    };

    std::string compressed = base64::decode(compressedBase64Encoded);

    auto roundtrip = [](const auto& compressed) {
        BSONObjBuilder builder;
        builder.appendBinData("data"_sd, compressed.size(), BinDataType::Column, compressed.data());
        BSONElement compressedFTDCElement = builder.done().firstElement();

        BSONColumnBuilder columnBuilder;
        BSONColumn column(compressedFTDCElement);
        for (auto&& decompressed : column) {
            columnBuilder.append(decompressed);
        }

        auto binData = columnBuilder.finalize();
        return std::string((const char*)binData.data, binData.length);
    };

    // Test that we can decompress and re-compress without any data loss.
    ASSERT_EQ(roundtrip(compressed), compressed);
}
#endif

class TestMaterializer {
public:
    using Element = std::
        variant<std::monostate, bool, int32_t, int64_t, double, Timestamp, Date_t, OID, StringData>;

    template <typename T>
    static Element materialize(BSONElementStorage& a, const T& val) {
        return val;
    }

    template <typename T>
    static Element materialize(BSONElementStorage& a, const BSONElement& val) {
        return std::monostate();
    }

    static Element materializePreallocated(const BSONElement& val) {
        return std::monostate();
    }


    static Element materializeMissing(BSONElementStorage& a) {
        return std::monostate();
    }

    static bool isMissing(Element elem) {
        return std::holds_alternative<std::monostate>(elem);
    }

    static int canonicalType(const Element& elem) {
        return elem.index();
    }

    static int compare(const Element& lhs,
                       const Element& rhs,
                       const StringDataComparator* comparator) {
        return 0;
    }
};

// Some compilers require that specializations be defined outside of class
template <>
TestMaterializer::Element TestMaterializer::materialize<BSONBinData>(BSONElementStorage& a,
                                                                     const BSONBinData& val) {
    return StringData((const char*)val.data, val.length);
}

template <>
TestMaterializer::Element TestMaterializer::materialize<BSONCode>(BSONElementStorage& a,
                                                                  const BSONCode& val) {
    return val.code;
}

template <>
TestMaterializer::Element TestMaterializer::materialize<bool>(BSONElementStorage& a,
                                                              const BSONElement& val) {
    return val.Bool();
}

template <>
TestMaterializer::Element TestMaterializer::materialize<int32_t>(BSONElementStorage& a,
                                                                 const BSONElement& val) {
    return (int32_t)val.Int();
}

template <>
TestMaterializer::Element TestMaterializer::materialize<int64_t>(BSONElementStorage& a,
                                                                 const BSONElement& val) {
    return (int64_t)val.Int();
}

template <>
TestMaterializer::Element TestMaterializer::materialize<double>(BSONElementStorage& a,
                                                                const BSONElement& val) {
    return val.Double();
}

template <>
TestMaterializer::Element TestMaterializer::materialize<unsigned long long>(
    BSONElementStorage& a, const BSONElement& val) {
    return val.date();
}

template <>
TestMaterializer::Element TestMaterializer::materialize<Date_t>(BSONElementStorage& a,
                                                                const BSONElement& val) {
    return val.Date();
}

template <>
TestMaterializer::Element TestMaterializer::materialize<OID>(BSONElementStorage& a,
                                                             const BSONElement& val) {
    return val.OID();
}

template <>
TestMaterializer::Element TestMaterializer::materialize<StringData>(BSONElementStorage& a,
                                                                    const BSONElement& val) {
    return val.valueStringData();
}

template <>
TestMaterializer::Element TestMaterializer::materialize<BSONBinData>(BSONElementStorage& a,
                                                                     const BSONElement& val) {
    int size = 0;
    return StringData(val.binData(size), size);
}

template <>
TestMaterializer::Element TestMaterializer::materialize<BSONCode>(BSONElementStorage& a,
                                                                  const BSONElement& val) {
    return val.valueStringData();
}

TEST_F(BSONColumnTest, TestCollector) {
    boost::intrusive_ptr allocator{new BSONElementStorage()};
    std::vector<TestMaterializer::Element> collection;
    Collector<TestMaterializer, std::vector<TestMaterializer::Element>> collector(collection,
                                                                                  allocator);
    size_t expectedSize = 0;

    collector.append(true);
    ASSERT_EQ(collection.size(), ++expectedSize);
    ASSERT_EQ(true, std::get<bool>(collection.back()));

    collector.append((int64_t)1);
    ASSERT_EQ(collection.size(), ++expectedSize);
    ASSERT_EQ(1, std::get<int64_t>(collection.back()));

    BSONBinData bsonBinData;
    bsonBinData.data = "foo";
    bsonBinData.length = 3;
    bsonBinData.type = BinDataGeneral;
    collector.append(bsonBinData);
    ASSERT_EQ(collection.size(), ++expectedSize);
    StringData result = std::get<StringData>(collection.back());
    ASSERT_EQ(3, result.size());
    ASSERT_EQ(0, memcmp("foo", result.data(), 3));

    BSONCode bsonCode;
    bsonCode.code = "bar";
    collector.append(bsonCode);
    ASSERT_EQ(collection.size(), ++expectedSize);
    result = std::get<StringData>(collection.back());
    ASSERT_EQ(3, result.size());
    ASSERT_EQ(0, memcmp("bar", result.data(), 3));

    BSONElement doubleVal = createElementDouble(2.0);
    collector.append<double>(doubleVal);
    ASSERT_EQ(collection.size(), ++expectedSize);
    ASSERT_EQ(2.0, std::get<double>(collection.back()));

    BSONElement stringVal = createElementString(StringData("bam", 3));
    collector.append<StringData>(stringVal);
    ASSERT_EQ(collection.size(), ++expectedSize);
    result = std::get<StringData>(collection.back());
    ASSERT_EQ(3, result.size());
    ASSERT_EQ(0, memcmp("bam", result.data(), 3));

    BSONElement codeVal = createElementCode(StringData("baz", 3));
    collector.append<BSONCode>(codeVal);
    ASSERT_EQ(collection.size(), ++expectedSize);
    result = std::get<StringData>(collection.back());
    ASSERT_EQ(3, result.size());
    ASSERT_EQ(0, memcmp("baz", result.data(), 3));

    BSONElement obj = createElementObj(BSON("x" << 1));
    collector.appendPreallocated(obj);
    ASSERT_EQ(collection.size(), ++expectedSize);
    ASSERT_EQ(std::monostate(), std::get<std::monostate>(collection.back()));

    collector.appendMissing();
    ASSERT_EQ(collection.size(), ++expectedSize);
    ASSERT_EQ(std::monostate(), std::get<std::monostate>(collection.back()));
}


}  // namespace
}  // namespace mongo::bsoncolumn
