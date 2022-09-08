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

#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/bson/util/bsoncolumnbuilder.h"
#include "mongo/bson/util/simple8b_type_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/base64.h"


namespace mongo {
namespace {

class BSONColumnTest : public unittest::Test {
public:
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

    BSONElement createRegex(StringData options = "") {
        BSONObjBuilder ob;
        ob.appendRegex("0"_sd, options);
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
            *Simple8bTypeUtil::encodeString(val.valueStringData()) -
            *Simple8bTypeUtil::encodeString(prev.valueStringData()));
    }

    static uint64_t deltaInt32(BSONElement val, BSONElement prev) {
        return Simple8bTypeUtil::encodeInt64(val.Int() - prev.Int());
    }

    static uint64_t deltaInt64(BSONElement val, BSONElement prev) {
        return Simple8bTypeUtil::encodeInt64(val.Long() - prev.Long());
    }

    static uint64_t deltaDouble(BSONElement val, BSONElement prev, double scaleFactor) {
        uint8_t scaleIndex = 0;
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
        Simple8bBuilder<uint64_t> b([](uint64_t block) {});
        return b.append(val);
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
            deltas.push_back(deltaInt64(*begin, prev));
            prev = *begin;
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

    static uint64_t deltaBool(BSONElement val, BSONElement prev) {
        return Simple8bTypeUtil::encodeInt64(val.Bool() - prev.Bool());
    }

    static uint64_t deltaOfDeltaDate(BSONElement val, BSONElement prev, BSONElement prevprev) {
        int64_t delta = val.Date().toMillisSinceEpoch() - prev.Date().toMillisSinceEpoch();
        int64_t prevDelta = prev.Date().toMillisSinceEpoch() - prevprev.Date().toMillisSinceEpoch();
        return deltaOfDelta(delta, prevDelta);
    }

    static void appendLiteral(BufBuilder& builder, BSONElement elem) {
        // BSON Type byte
        builder.appendChar(elem.type());

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
        Simple8bBuilder<T> s8bBuilder([&builder](uint64_t block) {
            builder.appendNum(block);
            return true;
        });
        if (val) {
            s8bBuilder.append(*val);
        } else {
            s8bBuilder.skip();
        }

        s8bBuilder.flush();
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
        Simple8bBuilder<T> s8bBuilder([&builder](uint64_t block) {
            builder.appendNum(block);
            return true;
        });
        for (auto val : vals) {
            if (val) {
                s8bBuilder.append(*val);
            } else {
                s8bBuilder.skip();
            }
        }
        s8bBuilder.flush();
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

    static void appendEOO(BufBuilder& builder) {
        builder.appendChar(EOO);
    }

    static void verifyBinary(BSONBinData columnBinary, const BufBuilder& expected) {
        ASSERT_EQ(columnBinary.type, BinDataType::Column);


        auto buf = expected.buf();
        ASSERT_EQ(columnBinary.length, expected.len());
        ASSERT_EQ(memcmp(columnBinary.data, buf, columnBinary.length), 0);
    }

    static void verifyDecompression(const BufBuilder& columnBinary,
                                    const std::vector<BSONElement>& expected) {
        BSONBinData bsonBinData;
        bsonBinData.data = columnBinary.buf();
        bsonBinData.length = columnBinary.len();
        bsonBinData.type = Column;
        verifyDecompression(bsonBinData, expected);
    }

    static void verifyDecompression(BSONBinData columnBinary,
                                    const std::vector<BSONElement>& expected) {
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
                BSONElement other = *(it++);
                ASSERT(elem.binaryEqualValues(other));
            }
        }

        // Verify that we can traverse BSONColumn and extract values on the first pass
        {
            BSONColumn col(columnElement);

            auto it = col.begin();
            for (auto elem : expected) {
                BSONElement other = *(it++);
                ASSERT(elem.binaryEqualValues(other));
            }
        }

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

            for (int i = (int)expected.size() - 1; i >= 0; --i) {
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
                ASSERT_EQ(&*it1, &*it2);  // Iterators should point to same reference
            }

            ASSERT(it1 == it2);
        }
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

private:
    std::forward_list<BSONObj> _elementMemory;
};

TEST_F(BSONColumnTest, Empty) {
    BSONColumnBuilder cb("test"_sd);

    BufBuilder expected;
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {});
}

TEST_F(BSONColumnTest, BasicValue) {
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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

TEST_F(BSONColumnTest, OnlySkip) {
    BSONColumnBuilder cb("test"_sd);

    cb.skip();

    BufBuilder expected;
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, boost::none);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, {BSONElement()});
}

TEST_F(BSONColumnTest, ValueAfterSkip) {
    BSONColumnBuilder cb("test"_sd);

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


TEST_F(BSONColumnTest, LargeDeltaIsLiteral) {
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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

TEST_F(BSONColumnTest, DoubleSameScale) {
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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

TEST_F(BSONColumnTest, DoubleDecreaseScaleAfterBlock) {
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

    std::vector<BSONElement> elems;
    elems.push_back(createElementDouble(1.12345671));
    elems.push_back(createElementDouble(2));
    elems.push_back(BSONElement());
    elems.push_back(BSONElement());
    elems.push_back(createElementDouble(3));

    for (const auto& elem : elems) {
        if (!elem.eoo()) {
            cb.append(elem);
        } else {
            cb.skip();
        }
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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

    std::vector<BSONElement> elems;
    elems.push_back(createElementDouble(1.12345671));
    elems.push_back(createElementDouble(2));
    elems.push_back(BSONElement());
    elems.push_back(BSONElement());
    elems.push_back(createElementDouble(1.12345671));

    for (const auto& elem : elems) {
        if (!elem.eoo()) {
            cb.append(elem);
        } else {
            cb.skip();
        }
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

TEST_F(BSONColumnTest, DoubleUnscalable) {
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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

TEST_F(BSONColumnTest, Decimal128Base) {
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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

TEST_F(BSONColumnTest, RegexAfterChangeBack) {
    BSONColumnBuilder cb("test"_sd);

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

TEST_F(BSONColumnTest, DBRefBasic) {
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);

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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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

TEST_F(BSONColumnTest, BinDataEqualTo16) {
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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

TEST_F(BSONColumnTest, StringBase) {
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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

TEST_F(BSONColumnTest, StringMultiType) {
    BSONColumnBuilder cb("test"_sd);
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

TEST_F(BSONColumnTest, CodeBase) {
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
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
    BSONColumnBuilder cb("test"_sd);
    std::vector<BSONElement> elems = {createElementArray(BSON_ARRAY(1 << 2)),
                                      createElementArray(BSON_ARRAY(1 << 2 << 3))};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    for (auto elem : elems) {
        appendLiteral(expected, elem);
    }
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, ArrayEqual) {
    BSONColumnBuilder cb("test"_sd);
    auto elemObj = createElementArray(BSON_ARRAY("a"
                                                 << "b"));
    std::vector<BSONElement> elems = {elemObj, elemObj};

    for (auto elem : elems) {
        cb.append(elem);
    }

    BufBuilder expected;
    appendLiteral(expected, elemObj);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, Interleaved) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << 2)),
                                          createElementObj(BSON("x" << 1 << "y" << 3)),
                                          createElementObj(BSON("x" << 1 << "y" << 3)),
                                          BSONElement(),
                                          createElementObj(BSON("y" << 4)),
                                          createElementObj(BSON("x" << 1 << "y" << 3))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
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
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedArray) {
    BSONColumnBuilder cb("test"_sd, true);
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
    BSONColumnBuilder cb("test"_sd, true);
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
    BSONColumnBuilder cb("test"_sd, true);
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
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {createElementInt32(1),
                                          createElementObj(BSON("x" << 1 << "y" << 2)),
                                          createElementObj(BSON("x" << 1 << "y" << 3)),
                                          createElementObj(BSON("x" << 2 << "y" << 4))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendLiteral(expected, elems.front());
        appendInterleavedStartFunc(expected, elems[1].Obj());
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
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedLevels) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("root" << BSON("x" << 1) << "y" << 2)),
            createElementObj(BSON("root" << BSON("x" << 2) << "y" << 5)),
            createElementObj(BSON("root" << BSON("x" << 2) << "y" << 5))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
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
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedDoubleDifferentScale) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1.0 << "y" << 2.0)),
                                          createElementObj(BSON("x" << 1.1 << "y" << 3.0)),
                                          createElementObj(BSON("x" << 1.2 << "y" << 2.0)),
                                          createElementObj(BSON("x" << 1.0)),
                                          createElementObj(BSON("x" << 1.5 << "y" << 2.0))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
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
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedDoubleIncreaseScaleFromDeltaNoRescale) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

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
        appendInterleavedStartFunc(expected, elems.front().Obj());

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
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedScalarToObject) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("x" << 1)),
            createElementObj(BSON("x" << 2)),
            createElementObj(BSON("x" << BSON("y" << 1 << "z" << 1))),
            createElementObj(BSON("x" << BSON("y" << 2 << "z" << 3)))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[2].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[3].Obj()["x"_sd].Obj()["y"_sd],
                                           elems[2].Obj()["x"_sd].Obj()["y"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[3].Obj()["x"_sd].Obj()["z"_sd],
                                           elems[2].Obj()["x"_sd].Obj()["z"_sd])},
                               1);
        appendEOO(expected);

        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
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
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

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
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
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
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedWithEmptySubObj) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj())),
            createElementObj(BSON("x" << 2 << "y" << BSONObjBuilder().obj())),
            createElementObj(BSON("x" << 3 << "y" << BSONObjBuilder().obj()))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
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
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedRemoveEmptySubObj) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj())),
            createElementObj(BSON("x" << 2 << "y" << BSONObjBuilder().obj())),
            createElementObj(BSON("x" << 3))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[2].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);

        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedAddEmptySubObj) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("x" << 3)),
            createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj()))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[1].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);

        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedAddEmptySubArray) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 3)),
        createElementObj(BSON("x" << 1 << "y" << BSONArrayBuilder().arr()))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
}

TEST_F(BSONColumnTest, InterleavedSchemaChange) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << 2)),
                                          createElementObj(BSON("x" << 1 << "y" << 3)),
                                          createElementObj(BSON("x" << 1 << "y" << 3.0)),
                                          createElementObj(BSON("x" << 1 << "y" << 4.0))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd]),
                                deltaInt32(elems[2].Obj()["x"_sd], elems[1].Obj()["x"_sd]),
                                deltaInt32(elems[3].Obj()["x"_sd], elems[2].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
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
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedObjectSchemaChange) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << BSON("z" << 2))),
                                          createElementObj(BSON("x" << 1 << "y" << BSON("z" << 3))),
                                          createElementObj(BSON("x" << 1 << "y" << 3))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["y"_sd].Obj()["z"_sd],
                                           elems[0].Obj()["y"_sd].Obj()["z"_sd])},
                               1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[2].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);
        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedObjectNameChange) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("x" << 1 << "y" << BSON("z" << 2))),
            createElementObj(BSON("x" << 1 << "y2" << BSON("z" << 3)))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(
            expected, BSON("x" << 1 << "y" << BSON("z" << 2) << "y2" << BSON("z" << 3)));
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
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
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedObjectEmptyObjChange) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("x" << 1 << "y" << BSON("z" << 2))),
            createElementObj(BSON("x" << 1 << "y" << BSON("z" << 3))),
            createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj()))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["y"_sd].Obj()["z"_sd],
                                           elems[0].Obj()["y"_sd].Obj()["z"_sd])},
                               1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[2].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);
        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedObjectEmptyArrayChange) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSON("z" << 2))),
        createElementObj(BSON("x" << 1 << "y" << BSON("z" << 3))),
        createElementObj(BSON("x" << 1 << "y" << BSONArrayBuilder().arr()))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyObjMiddle) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("x" << 1 << "z" << 2)),
            createElementObj(BSON("x" << 1 << "z" << 3)),
            createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj() << "z" << 4))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
                               1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[2].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);

        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyArrayMiddle) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "z" << 2)),
        createElementObj(BSON("x" << 1 << "z" << 3)),
        createElementObj(BSON("x" << 1 << "y" << BSONArrayBuilder().arr() << "z" << 4))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyObjUnderObj) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("x" << 1 << "z" << 2)),
            createElementObj(BSON("x" << 1 << "z" << 3)),
            createElementObj(
                BSON("x" << 1 << "y" << BSON("y1" << BSONObjBuilder().obj()) << "z" << 4))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
                               1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[2].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);

        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyArrayUnderObj) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "z" << 2)),
        createElementObj(BSON("x" << 1 << "z" << 3)),
        createElementObj(
            BSON("x" << 1 << "y" << BSON("y1" << BSONArrayBuilder().arr()) << "z" << 4))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("x" << 1 << "y" << 2)),
            createElementObj(BSON("x" << 1 << "y" << 3)),
            createElementObj(BSON("x" << 1 << "y" << 4 << "z" << BSONObjBuilder().obj()))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
                               1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[2].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);

        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyArrayEnd) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2)),
        createElementObj(BSON("x" << 1 << "y" << 3)),
        createElementObj(BSON("x" << 1 << "y" << 4 << "z" << BSONArrayBuilder().arr()))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("x" << 1 << "y" << 2)),
            createElementObj(BSON("x" << 1 << "y" << 3)),
            createElementObj(
                BSON("x" << 1 << "y" << 4 << "z" << BSON("z1" << BSONObjBuilder().obj())))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
                               1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[2].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);

        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedObjectNewEmptyArrayUnderObjEnd) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2)),
        createElementObj(BSON("x" << 1 << "y" << 3)),
        createElementObj(
            BSON("x" << 1 << "y" << 4 << "z" << BSON("z1" << BSONArrayBuilder().arr())))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2)),
        createElementObj(BSON("x" << 1 << "y" << 3)),
        createElementObj(
            BSON("x" << 1 << "y" << 4 << "z" << BSON_ARRAY(BSONArrayBuilder().arr())))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj() << "z" << 2)),
            createElementObj(BSON("x" << 1 << "y" << BSONObjBuilder().obj() << "z" << 3)),
            createElementObj(BSON("x" << 1 << "z" << 4))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
                               1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[2].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);

        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyArrayMiddle) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSONArrayBuilder().arr() << "z" << 2)),
        createElementObj(BSON("x" << 1 << "y" << BSONArrayBuilder().arr() << "z" << 3)),
        createElementObj(BSON("x" << 1 << "z" << 4))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementArray(BSON_ARRAY(1 << BSONObjBuilder().obj() << 2)),
        createElementArray(BSON_ARRAY(1 << BSONObjBuilder().obj() << 3)),
        createElementArray(BSON_ARRAY(1 << 4))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(
                BSON("x" << 1 << "y" << BSON("y1" << BSONObjBuilder().obj()) << "z" << 2)),
            createElementObj(
                BSON("x" << 1 << "y" << BSON("y1" << BSONObjBuilder().obj()) << "z" << 3)),
            createElementObj(BSON("x" << 1 << "z" << 4))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["z"_sd], elems[0].Obj()["z"_sd])},
                               1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[2].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);

        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyArrayUnderObj) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(
            BSON("x" << 1 << "y" << BSON("y1" << BSONArrayBuilder().arr()) << "z" << 2)),
        createElementObj(
            BSON("x" << 1 << "y" << BSON("y1" << BSONArrayBuilder().arr()) << "z" << 3)),
        createElementObj(BSON("x" << 1 << "z" << 4))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << BSON_ARRAY(BSONArrayBuilder().arr()) << "z" << 2)),
        createElementObj(BSON("x" << 1 << "y" << BSON_ARRAY(BSONArrayBuilder().arr()) << "z" << 3)),
        createElementObj(BSON("x" << 1 << "z" << 4))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("x" << 1 << "y" << 2 << "z" << BSONObjBuilder().obj())),
            createElementObj(BSON("x" << 1 << "y" << 3 << "z" << BSONObjBuilder().obj())),
            createElementObj(BSON("x" << 1 << "y" << 4))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
                               1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[2].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);

        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyArrayEnd) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(BSON("x" << 1 << "y" << 2 << "z" << BSONArrayBuilder().arr())),
        createElementObj(BSON("x" << 1 << "y" << 3 << "z" << BSONArrayBuilder().arr())),
        createElementObj(BSON("x" << 1 << "y" << 4))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementArray(BSON_ARRAY(1 << 2 << BSONArrayBuilder().arr())),
        createElementArray(BSON_ARRAY(1 << 3 << BSONArrayBuilder().arr())),
        createElementArray(BSON_ARRAY(1 << 4))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(
                BSON("x" << 1 << "y" << 2 << "z" << BSON("z1" << BSONObjBuilder().obj()))),
            createElementObj(
                BSON("x" << 1 << "y" << 3 << "z" << BSON("z1" << BSONObjBuilder().obj()))),
            createElementObj(BSON("x" << 1 << "y" << 4))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
                               1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[2].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);

        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedObjectMissingEmptyArrayUnderObjEnd) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(
            BSON("x" << 1 << "y" << 2 << "z" << BSON("z1" << BSONArrayBuilder().arr()))),
        createElementObj(
            BSON("x" << 1 << "y" << 3 << "z" << BSON("z1" << BSONArrayBuilder().arr()))),
        createElementObj(BSON("x" << 1 << "y" << 4))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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

TEST_F(BSONColumnTest, InterleavedArrayMissingEmptyObjUnderObjEnd) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementArray(BSON_ARRAY(1 << 2 << BSON("z1" << BSONObjBuilder().obj()))),
        createElementArray(BSON_ARRAY(1 << 3 << BSON("z1" << BSONObjBuilder().obj()))),
        createElementArray(BSON_ARRAY(1 << 4))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1 << "y" << 2)),
                                          createElementObj(BSON("x" << 1 << "y" << 3)),
                                          createElementInt32(1),
                                          createElementObj(BSON("x" << 2 << "y" << 2 << "z" << 2)),
                                          createElementObj(BSON("x" << 5 << "y" << 3 << "z" << 3))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["y"_sd], elems[0].Obj()["y"_sd])},
                               1);
        appendEOO(expected);
        appendLiteral(expected, elems[2]);
        appendInterleavedStartFunc(expected, elems[3].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[4].Obj()["x"_sd], elems[3].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[4].Obj()["y"_sd], elems[3].Obj()["y"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[4].Obj()["z"_sd], elems[3].Obj()["z"_sd])},
                               1);
        appendEOO(expected);
        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, ReenterInterleavedArrayRootToObj) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {createElementArray(BSON_ARRAY(1 << 2)),
                                      createElementArray(BSON_ARRAY(1 << 3)),
                                      createElementInt32(1),
                                      createElementObj(BSON("x" << 2 << "y" << 2 << "z" << 2)),
                                      createElementObj(BSON("x" << 5 << "y" << 3 << "z" << 3))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                          createElementObj(BSON("y" << 2)),
                                          createElementObj(BSON("z" << 3)),
                                          createElementObj(BSON("x" << 2)),
                                          createElementObj(BSON("y" << 3)),
                                          createElementObj(BSON("z" << 4))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected,
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
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedAlternatingMergeLeftThenRight) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {createElementObj(BSON("z" << 1)),
                                          createElementObj(BSON("y" << 2 << "z" << 2)),
                                          createElementObj(BSON("x" << 3 << "z" << 3))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected,
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
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedMergeWithUnrelatedArray) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(BSON("a" << BSON_ARRAY(1 << 2) << "z" << 1)),
        createElementObj(BSON("a" << BSON_ARRAY(1 << 2) << "y" << 2 << "z" << 2)),
        createElementObj(BSON("a" << BSON_ARRAY(1 << 2) << "x" << 3 << "z" << 3))};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
}

TEST_F(BSONColumnTest, InterleavedMergeWithScalarObjectMismatch) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        // Test that we can successfully build reference object when there are unrelated fields with
        // object and scalar mismatch.
        std::vector<BSONElement> elems = {
            createElementObj(BSON("z" << BSON("x" << 1))),
            createElementObj(BSON("y" << 1 << "z" << BSON("x" << 2)))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(
            expected,
            BSON("y" << elems[1].Obj()["y"_sd].Int() << "z" << elems[0].Obj()["z"_sd].Obj()));
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {boost::none, kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["z"_sd].Obj()["x"_sd],
                                           elems[0].Obj()["z"_sd].Obj()["x"_sd])},
                               1);
        appendEOO(expected);
        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedArrayAppend) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementArray(BSONArrayBuilder().arr()),
        createElementArray(BSON_ARRAY(1)),
        createElementArray(BSON_ARRAY(1 << 2)),
        createElementArray(BSON_ARRAY(1 << 2 << 3)),
        createElementArray(BSON_ARRAY(1 << 2 << 3 << 4)),
    };

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, InterleavedIncompatibleMerge) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                          createElementObj(BSON("x" << 2 << "y" << 2)),
                                          createElementObj(BSON("y" << 3 << "x" << 3))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected,
                                   BSON("x" << elems[0].Obj().firstElement().Int() << "y"
                                            << elems[1].Obj()["y"_sd].Int()));
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected,
                               {kDeltaForBinaryEqualValues,
                                deltaInt32(elems[1].Obj()["x"_sd], elems[0].Obj()["x"_sd])},
                               1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {boost::none, kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);
        appendInterleavedStartFunc(expected, elems[2].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);
        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedIncompatibleMergeMiddle) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(BSON("a" << 1 << "x" << 2 << "y" << 2 << "b" << 2)),
            createElementObj(BSON("a" << 1 << "y" << 3 << "x" << 3 << "b" << 2))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;

        appendInterleavedStartFunc(expected, elems[0].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);

        appendInterleavedStartFunc(expected, elems[1].Obj());
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
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedIncompatibleAfterDeterminedReference) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                          createElementObj(BSON("x" << 2)),
                                          createElementObj(BSON("x" << 3)),
                                          createElementObj(BSON("x" << 4)),
                                          createElementObj(BSON("x" << 5)),
                                          createElementObj(BSON("x" << 6)),
                                          createElementObj(BSON("x" << 0 << "y" << 0))};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
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
        appendInterleavedStartFunc(expected, elems[6].Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);

        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedSkipAfterEmptySubObj) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {
            createElementObj(
                BSON("x" << 1 << "y" << 2 << "z" << BSON("z1" << BSONObjBuilder().obj()))),
            BSONElement()};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
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
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, InterleavedSkipAfterEmptySubArray) {
    BSONColumnBuilder cb("test"_sd, true);

    std::vector<BSONElement> elems = {
        createElementObj(
            BSON("x" << 1 << "y" << 2 << "z" << BSON("z1" << BSONArrayBuilder().arr()))),
        BSONElement()};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
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
    BSONColumnBuilder cb("test"_sd);

    std::vector<BSONElement> elems = {createElementObj(BSONObjBuilder().obj()),
                                      createElementObj(BSONObjBuilder().obj())};

    for (auto elem : elems) {
        if (!elem.eoo())
            cb.append(elem);
        else
            cb.skip();
    }

    BufBuilder expected;
    appendLiteral(expected, elems.front());
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
    appendEOO(expected);

    auto binData = cb.finalize();
    verifyBinary(binData, expected);
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, ObjectEmptyAfterNonEmpty) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

        std::vector<BSONElement> elems = {createElementObj(BSON("x" << 1)),
                                          createElementObj(BSONObjBuilder().obj())};

        for (auto elem : elems) {
            if (!elem.eoo())
                cb.append(elem);
            else
                cb.skip();
        }

        BufBuilder expected;
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
        appendEOO(expected);
        appendLiteral(expected, elems[1]);
        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, ObjectWithOnlyEmptyObjsDoesNotStartInterleaving) {
    BSONColumnBuilder cb("test"_sd);

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
    verifyDecompression(binData, elems);
}

TEST_F(BSONColumnTest, ObjectWithOnlyEmptyObjsDoesNotStartInterleavingFromDetermine) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

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
        appendInterleavedStartFunc(expected, elems.front().Obj());
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlock64(expected, kDeltaForBinaryEqualValues);
        appendEOO(expected);

        appendLiteral(expected, elems[1]);
        appendEOO(expected);

        auto binData = cb.finalize();
        verifyBinary(binData, expected);
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, ObjectWithOnlyEmptyObjsDoesNotStartInterleavingFromAppending) {
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);

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
        appendInterleavedStartFunc(expected, elems.front().Obj());
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
        verifyDecompression(binData, elems);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);
}

TEST_F(BSONColumnTest, NonZeroRLEInFirstBlockAfterSimple8bBlocks) {
    BSONColumnBuilder cb("test"_sd);

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
    Simple8bBuilder<uint64_t> s8bBuilder([&](uint64_t block) {
        if (blockCount++ == 16) {
            appendSimple8bControl(expected, 0b1000, 0b0000);
        }
        expected.appendNum(block);
        return true;
    });

    for (auto delta : deltas) {
        s8bBuilder.append(*delta);
    }
    s8bBuilder.flush();
    appendEOO(expected);

    // We should now have 16 regular Simple8b blocks and then a 17th using RLE at the end.
    ASSERT_EQ(blockCount, 17);

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
    // A Simple8b block with an invalid selector doesn't throw an error, but make sure we can handle
    // it gracefully. Check so we don't read out of bounds and can iterate.

    auto elem = createElementInt32(0);

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    uint64_t invalidSimple8b = 0;
    expected.appendNum(invalidSimple8b);
    appendEOO(expected);

    BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
    for (auto it = col.begin(), e = col.end(); it != e; ++it) {
    }
}

TEST_F(BSONColumnTest, NoLiteralStart) {
    // Starting the stream with a delta block doesn't throw an error. Make sure we handle it
    // gracefully even if the values we extracted may not be meaningful. Check so we don't read out
    // of bounds and can iterate.

    auto elem = createElementInt32(0);

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    uint64_t invalidSimple8b = 0;
    expected.appendNum(invalidSimple8b);
    appendEOO(expected);

    BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
    for (auto it = col.begin(), e = col.end(); it != e; ++it) {
    }
}

TEST_F(BSONColumnTest, InvalidInterleavedCount) {
    // This test sets up an interleaved reference object with two fields but only provides one
    // interleaved substream.
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);
        BufBuilder expected;
        appendInterleavedStartFunc(expected, BSON("a" << 1 << "b" << 1));
        appendSimple8bControl(expected, 0b1000, 0b0000);
        appendSimple8bBlocks64(expected, {kDeltaForBinaryEqualValues}, 1);
        appendEOO(expected);
        appendEOO(expected);

        BSONColumn col(createBSONColumn(expected.buf(), expected.len()));
        ASSERT_THROWS(std::distance(col.begin(), col.end()), DBException);
    };

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);

    BSONColumnBuilder cb("test"_sd, true);
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
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
        BSONColumnBuilder cb("test"_sd, arrayCompression);
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

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);

    BSONColumnBuilder cb("test"_sd, true);
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
    auto test = [&](bool arrayCompression, auto appendInterleavedStartFunc) {
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

    test(false, appendInterleavedStartLegacy);
    test(true, appendInterleavedStart);

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
    BSONColumnBuilder cb("test");
    ASSERT_THROWS_CODE(cb.append(createElementMinKey()), DBException, ErrorCodes::InvalidBSONType);

    BufBuilder expected;
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
}

TEST_F(BSONColumnTest, AppendMaxKey) {
    BSONColumnBuilder cb("test");
    ASSERT_THROWS_CODE(cb.append(createElementMaxKey()), DBException, ErrorCodes::InvalidBSONType);

    BufBuilder expected;
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
}

TEST_F(BSONColumnTest, AppendMinKeyInSubObj) {
    BSONColumnBuilder cb("test");

    BSONObjBuilder obj;
    {
        BSONObjBuilder builder = obj.subobjStart("root");
        builder.append(createElementMinKey());
    }

    ASSERT_THROWS_CODE(
        cb.append(createElementObj(obj.obj())), DBException, ErrorCodes::InvalidBSONType);

    BufBuilder expected;
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
}

TEST_F(BSONColumnTest, AppendMinKeyInSubObjAfterInterleaveStart) {
    BSONColumnBuilder cb("test");

    BSONObjBuilder obj;
    {
        BSONObjBuilder builder = obj.subobjStart("root");
        builder.append(createElementMinKey());
    }

    cb.append(createElementObj(BSON("root" << BSON("0" << 1))));
    ASSERT_THROWS_CODE(
        cb.append(createElementObj(obj.obj())), DBException, ErrorCodes::InvalidBSONType);
}

TEST_F(BSONColumnTest, AppendMinKeyInSubObjAfterInterleaveStartInAppendMode) {
    BSONColumnBuilder cb("test");

    BSONObjBuilder obj;
    {
        BSONObjBuilder builder = obj.subobjStart("root");
        builder.append(createElementMinKey());
    }

    cb.append(createElementObj(BSON("root" << BSON("0" << 1))));
    cb.append(createElementObj(BSON("root" << BSON("0" << 1))));
    cb.append(createElementObj(BSON("root" << BSON("0" << 1))));
    cb.append(createElementObj(BSON("root" << BSON("0" << 1))));
    cb.append(createElementObj(BSON("root" << BSON("0" << 1))));
    cb.append(createElementObj(BSON("root" << BSON("0" << 1))));
    cb.append(createElementObj(BSON("root" << BSON("0" << 1))));
    ASSERT_THROWS_CODE(
        cb.append(createElementObj(obj.obj())), DBException, ErrorCodes::InvalidBSONType);
}

TEST_F(BSONColumnTest, AppendMinKeyInSubObjAfterMerge) {
    BSONColumnBuilder cb("test");

    BSONObjBuilder obj;
    {
        BSONObjBuilder builder = obj.subobjStart("root");
        builder.append("a", "asd");
        builder.append(createElementMinKey());
    }

    cb.append(createElementObj(BSON("root" << BSON("0" << 1))));
    // Make sure we throw InvalidBSONType even if we would detect that "a" needs to be merged before
    // observing the MinKey.
    ASSERT_THROWS_CODE(
        cb.append(createElementObj(obj.obj())), DBException, ErrorCodes::InvalidBSONType);
}

// The large literal emits this on Visual Studio: Fatal error C1091: compiler limit: string exceeds
// 65535 bytes in length
#if !defined(_MSC_VER) || _MSC_VER >= 1929
TEST_F(BSONColumnTest, FTDCRoundTrip) {
    StringData compressedBase64Encoded = {
#include "mongo/bson/util/bson_column_compressed_data.inl"
    };

    std::string compressed = base64::decode(compressedBase64Encoded);

    auto roundtrip = [](const auto& compressed, bool arrayCompression) {
        BSONObjBuilder builder;
        builder.appendBinData("data"_sd, compressed.size(), BinDataType::Column, compressed.data());
        BSONElement compressedFTDCElement = builder.done().firstElement();

        BSONColumnBuilder columnBuilder("", arrayCompression);
        BSONColumn column(compressedFTDCElement);
        for (auto&& decompressed : column) {
            if (!decompressed.eoo()) {
                columnBuilder.append(decompressed);
            } else {
                columnBuilder.skip();
            }
        }

        auto binData = columnBuilder.finalize();
        return std::string((const char*)binData.data, binData.length);
    };

    // Test that we can decompress and re-compress without any data loss.
    ASSERT_EQ(roundtrip(compressed, false), compressed);

    // Re-compress with array support. This will change the interleaving start control bytes.
    std::string compressedArraySupport = roundtrip(compressed, true);

    // Verify that we can round-trip with array support
    ASSERT_EQ(roundtrip(compressedArraySupport, true), compressedArraySupport);

    // Last verify that we can roundtrip back to without array support. This tests that there was no
    // data loss when using array support.
    ASSERT_EQ(roundtrip(compressedArraySupport, false), compressed);
}
#endif

}  // namespace
}  // namespace mongo
