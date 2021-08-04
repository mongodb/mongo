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

#include "mongo/bson/util/bsoncolumnbuilder.h"
#include "mongo/bson/util/simple8b_type_util.h"

#include "mongo/unittest/unittest.h"

#include <boost/optional/optional_io.hpp>

namespace mongo {
namespace {

class BSONColumnTest : public unittest::Test {
public:
    BSONElement createElementInt32(int32_t val) {
        BSONObjBuilder ob;
        ob.append("0"_sd, val);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createElementInt64(int64_t val) {
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
        BSONObjBuilder ob;
        ob.append("0"_sd, val);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

    BSONElement createTimestamp(Timestamp val) {
        BSONObjBuilder ob;
        ob.append("0"_sd, val);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
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

    static uint64_t deltaObjectId(BSONElement val, BSONElement prev) {
        return Simple8bTypeUtil::encodeInt64(Simple8bTypeUtil::encodeObjectId(val.OID()) -
                                             Simple8bTypeUtil::encodeObjectId(prev.OID()));
    }

    uint64_t deltaOfDeltaTimestamp(BSONElement val, BSONElement prev) {
        return Simple8bTypeUtil::encodeInt64(val.timestamp().asULL() - prev.timestamp().asULL());
    }

    static uint64_t deltaOfDeltaTimestamp(BSONElement val, BSONElement prev, BSONElement prevprev) {
        int64_t prevTimestampDelta = prev.timestamp().asULL() - prevprev.timestamp().asULL();
        int64_t currTimestampDelta = val.timestamp().asULL() - prev.timestamp().asULL();
        return Simple8bTypeUtil::encodeInt64(currTimestampDelta - prevTimestampDelta);
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

    static void appendSimple8bBlock(BufBuilder& builder, boost::optional<uint64_t> val) {
        auto prev = builder.len();
        Simple8bBuilder<uint64_t> s8bBuilder([&builder](uint64_t block) {
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

    static void appendSimple8bBlocks(BufBuilder& builder,
                                     const std::vector<boost::optional<uint64_t>>& vals,
                                     uint32_t expectedNum) {
        auto prev = builder.len();
        Simple8bBuilder<uint64_t> s8bBuilder([&builder](uint64_t block) {
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

    static void appendEOO(BufBuilder& builder) {
        builder.appendChar(EOO);
    }

    static void verifyBinary(BSONBinData columnBinary, const BufBuilder& expected) {
        ASSERT_EQ(columnBinary.type, BinDataType::Column);
        ASSERT_EQ(columnBinary.length, expected.len());

        auto buf = expected.buf();
        ASSERT_EQ(memcmp(columnBinary.data, buf, columnBinary.length), 0);
    }

private:
    std::forward_list<BSONObj> _elementMemory;
};

TEST_F(BSONColumnTest, BasicValue) {
    BSONColumnBuilder cb("test"_sd);

    auto elem = createElementInt32(1);
    cb.append(elem);
    cb.append(elem);

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock(expected, 0);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
}

TEST_F(BSONColumnTest, BasicSkip) {
    BSONColumnBuilder cb("test"_sd);

    auto elem = createElementInt32(1);
    cb.append(elem);
    cb.skip();

    BufBuilder expected;
    appendLiteral(expected, elem);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock(expected, boost::none);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
}

TEST_F(BSONColumnTest, OnlySkip) {
    BSONColumnBuilder cb("test"_sd);

    cb.skip();

    BufBuilder expected;
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock(expected, boost::none);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
}

TEST_F(BSONColumnTest, ValueAfterSkip) {
    BSONColumnBuilder cb("test"_sd);

    auto elem = createElementInt32(1);
    cb.skip();
    cb.append(elem);

    BufBuilder expected;
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock(expected, boost::none);
    appendLiteral(expected, elem);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlock(expected, deltaInt64(zero, zero));
    appendLiteral(expected, large);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock(expected, deltaInt64(large, large));
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlocks(expected, deltaInt64(chunk1Begin, chunk1End, elems.front()), 16);

    appendSimple8bControl(expected, 0b1000, 0b0010);
    appendSimple8bBlocks(expected, deltaInt64(chunk1End, elems.end(), *(chunk1End - 1)), 3);

    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlock(expected, 0);
    appendLiteral(expected, elemInt64);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlock(expected, 0);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlock(expected, deltaDouble(d2, d1, 1));
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlocks(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 1), 1);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlock(expected, deltaDouble(d2, d1, 10));
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlock(expected, boost::none);
    appendLiteral(expected, d1);
    appendSimple8bControl(expected, 0b1010, 0b0000);
    appendSimple8bBlock(expected, deltaDouble(d2, d1, 10));
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlocks(expected, expectedVals, 1);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlocks(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 10), 1);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlocks(expected, deltaDouble(deltaBegin, deltaEnd, elems.front(), 10), 1);

    appendSimple8bControl(expected, 0b1101, 0b0000);
    appendSimple8bBlocks(
        expected, deltaDouble(deltaEnd, elems.end(), *(deltaEnd - 1), 100000000), 1);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlocks(expected, deltaDouble(deltaBegin, deltaEnd, elems.front(), 100000000), 1);

    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bBlocks(expected, deltaDouble(deltaEnd, elems.end(), *(deltaEnd - 1), 1), 1);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlocks(expected, deltaDouble(deltaBegin, deltaEnd, elems.front(), 100000000), 1);

    appendSimple8bControl(expected, 0b1001, 0b0000);
    appendSimple8bBlocks(expected, deltaDouble(deltaEnd, elems.end(), *deltaBegin, 1), 1);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlocks(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 100000000), 2);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlocks(
        expected, deltaDouble(elems.begin() + 1, elems.end(), elems.front(), 100000000), 2);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlocks(expected, expectedVals, 1);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
        appendSimple8bBlock(expected, delta);
    } else {
        appendLiteral(expected, nan);
    }

    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
        appendSimple8bBlock(expected, delta);
    } else {
        appendLiteral(expected, nan);
    }
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
        appendSimple8bBlock(expected, delta);
    } else {
        appendLiteral(expected, inf);
    }
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
        appendSimple8bBlock(expected, delta);
    } else {
        appendLiteral(expected, denorm);
    }
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlock(expected, deltaDoubleMemory(e2, e1));
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
}

TEST_F(BSONColumnTest, BasicObjectId) {
    BSONColumnBuilder cb("test"_sd);

    auto first = createObjectId(OID("112233445566778899AABBCC"));
    // Increment the lower byte for timestamp and counter.
    auto second = createObjectId(OID("112234445566778899AABBEE"));
    auto third = createObjectId(OID("112234445566778899AABBFF"));

    cb.append(first);
    cb.append(second);
    cb.append(second);
    cb.append(third);

    BufBuilder expected;
    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    std::vector<boost::optional<uint64_t>> expectedDeltas{
        deltaObjectId(second, first), deltaObjectId(second, second), deltaObjectId(third, second)};
    appendSimple8bBlocks(expected, expectedDeltas, 1);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlock(expected, deltaObjectId(second, first));

    appendLiteral(expected, elemInt32);

    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock(expected, deltaObjectId(second, first));

    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlocks(expected, expectedDeltaOfDeltas, 1);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlocks(expected, expectedDeltaOfDeltas, 1);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlock(expected, deltaOfDeltaTimestamp(second, first));

    appendLiteral(expected, elemInt32);

    appendLiteral(expected, first);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    appendSimple8bBlock(expected, deltaOfDeltaTimestamp(second, first));

    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
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

    verifyBinary(cb.finalize(), expected);
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
    appendSimple8bBlock(expected, deltaOfDeltaTimestamp(zero, zero));
    appendLiteral(expected, large);
    appendSimple8bControl(expected, 0b1000, 0b0000);
    std::vector<boost::optional<uint64_t>> expectedDeltaOfDeltas{
        deltaOfDeltaTimestamp(large, large), deltaOfDeltaTimestamp(semiLarge, large, large)};
    appendSimple8bBlocks(expected, expectedDeltaOfDeltas, 1);
    appendEOO(expected);

    verifyBinary(cb.finalize(), expected);
}

}  // namespace
}  // namespace mongo
