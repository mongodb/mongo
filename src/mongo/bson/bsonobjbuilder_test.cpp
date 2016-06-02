/**
 *    Copyright (C) 2013 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * tests for BSONObjBuilder
 */

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"

#include "mongo/unittest/unittest.h"
#include <sstream>

namespace mongo {
namespace {

using mongo::BSONElement;
using mongo::BSONObj;
using mongo::BSONObjBuilder;
using mongo::BSONType;
using mongo::BufBuilder;
using std::string;
using std::stringstream;

const long long maxEncodableInt = (1 << 30) - 1;
const long long minEncodableInt = -maxEncodableInt;

const long long maxInt = (std::numeric_limits<int>::max)();
const long long minInt = (std::numeric_limits<int>::min)();

const long long maxEncodableDouble = (1LL << 40) - 1;
const long long minEncodableDouble = -maxEncodableDouble;

const long long maxDouble = (1LL << std::numeric_limits<double>::digits) - 1;
const long long minDouble = -maxDouble;

const long long maxLongLong = (std::numeric_limits<long long>::max)();
const long long minLongLong = (std::numeric_limits<long long>::min)();

template <typename T>
void assertBSONTypeEquals(BSONType actual, BSONType expected, T value, int i) {
    if (expected != actual) {
        stringstream ss;
        ss << "incorrect type in bson object for " << (i + 1) << "-th test value " << value
           << ". actual: " << mongo::typeName(actual)
           << "; expected: " << mongo::typeName(expected);
        const string msg = ss.str();
        FAIL(msg);
    }
}

TEST(BSONObjBuilder, AppendInt64T) {
    auto obj = BSON("a" << int64_t{5} << "b" << int64_t{1ll << 40});
    ASSERT_EQ(obj["a"].type(), NumberLong);
    ASSERT_EQ(obj["b"].type(), NumberLong);
    ASSERT_EQ(obj["a"].Long(), 5);
    ASSERT_EQ(obj["b"].Long(), 1ll << 40);
}

/**
 * current conversion ranges in append(unsigned n)
 * dbl/int max/min in comments refer to max/min encodable constants
 *                  0 <= n <= uint_max          -----> int
 */

TEST(BSONObjBuilderTest, AppendUnsignedInt) {
    struct {
        unsigned int v;
        BSONType t;
    } data[] = {{0, mongo::NumberInt},
                {100, mongo::NumberInt},
                {maxEncodableInt, mongo::NumberInt},
                {maxEncodableInt + 1, mongo::NumberInt},
                {static_cast<unsigned int>(maxInt), mongo::NumberInt},
                {static_cast<unsigned int>(maxInt) + 1U, mongo::NumberInt},
                {(std::numeric_limits<unsigned int>::max)(), mongo::NumberInt},
                {0, mongo::Undefined}};
    for (int i = 0; data[i].t != mongo::Undefined; i++) {
        unsigned int v = data[i].v;
        BSONObjBuilder b;
        b.append("a", v);
        BSONObj o = b.obj();
        ASSERT_EQUALS(o.nFields(), 1);
        BSONElement e = o.getField("a");
        unsigned int n = e.numberLong();
        ASSERT_EQUALS(n, v);
        assertBSONTypeEquals(e.type(), data[i].t, v, i);
    }
}

/**
 * current conversion ranges in appendIntOrLL(long long n)
 * dbl/int max/min in comments refer to max/min encodable constants
 *                       n <  dbl_min            -----> long long
 *            dbl_min <= n <  int_min            -----> double
 *            int_min <= n <= int_max            -----> int
 *            int_max <  n <= dbl_max            -----> double
 *            dbl_max <  n                       -----> long long
 */

TEST(BSONObjBuilderTest, AppendIntOrLL) {
    struct {
        long long v;
        BSONType t;
    } data[] = {{0, mongo::NumberInt},
                {-100, mongo::NumberInt},
                {100, mongo::NumberInt},
                {-(maxInt / 2 - 1), mongo::NumberInt},
                {maxInt / 2 - 1, mongo::NumberInt},
                {-(maxInt / 2), mongo::NumberLong},
                {maxInt / 2, mongo::NumberLong},
                {minEncodableInt, mongo::NumberLong},
                {maxEncodableInt, mongo::NumberLong},
                {minEncodableInt - 1, mongo::NumberLong},
                {maxEncodableInt + 1, mongo::NumberLong},
                {minInt, mongo::NumberLong},
                {maxInt, mongo::NumberLong},
                {minInt - 1, mongo::NumberLong},
                {maxInt + 1, mongo::NumberLong},
                {minLongLong, mongo::NumberLong},
                {maxLongLong, mongo::NumberLong},
                {0, mongo::Undefined}};
    for (int i = 0; data[i].t != mongo::Undefined; i++) {
        long long v = data[i].v;
        BSONObjBuilder b;
        b.appendIntOrLL("a", v);
        BSONObj o = b.obj();
        ASSERT_EQUALS(o.nFields(), 1);
        BSONElement e = o.getField("a");
        long long n = e.numberLong();
        ASSERT_EQUALS(n, v);
        assertBSONTypeEquals(e.type(), data[i].t, v, i);
    }
}

/**
 * current conversion ranges in appendNumber(size_t n)
 * dbl/int max/min in comments refer to max/min encodable constants
 *                  0 <= n <= int_max            -----> int
 *            int_max <  n                       -----> long long
 */

TEST(BSONObjBuilderTest, AppendNumberSizeT) {
    struct {
        size_t v;
        BSONType t;
    } data[] = {{0, mongo::NumberInt},
                {100, mongo::NumberInt},
                {maxEncodableInt, mongo::NumberInt},
                {maxEncodableInt + 1, mongo::NumberLong},
                {size_t(maxInt), mongo::NumberLong},
                {size_t(maxInt) + 1U, mongo::NumberLong},
                {(std::numeric_limits<size_t>::max)(), mongo::NumberLong},
                {0, mongo::Undefined}};
    for (int i = 0; data[i].t != mongo::Undefined; i++) {
        size_t v = data[i].v;
        BSONObjBuilder b;
        b.appendNumber("a", v);
        BSONObj o = b.obj();
        ASSERT_EQUALS(o.nFields(), 1);
        BSONElement e = o.getField("a");
        size_t n = e.numberLong();
        ASSERT_EQUALS(n, v);
        assertBSONTypeEquals(e.type(), data[i].t, v, i);
    }
}

/**
 * current conversion ranges in appendNumber(long long n)
 * dbl/int max/min in comments refer to max/min encodable constants
 *                       n <  dbl_min            -----> long long
 *            dbl_min <= n <  int_min            -----> double
 *            int_min <= n <= int_max            -----> int
 *            int_max <  n <= dbl_max            -----> double
 *            dbl_max <  n                       -----> long long
 */

TEST(BSONObjBuilderTest, AppendNumberLongLong) {
    struct {
        long long v;
        BSONType t;
    } data[] = {{0, mongo::NumberInt},
                {-100, mongo::NumberInt},
                {100, mongo::NumberInt},
                {minEncodableInt, mongo::NumberInt},
                {maxEncodableInt, mongo::NumberInt},
                {minEncodableInt - 1, mongo::NumberDouble},
                {maxEncodableInt + 1, mongo::NumberDouble},
                {minInt, mongo::NumberDouble},
                {maxInt, mongo::NumberDouble},
                {minInt - 1, mongo::NumberDouble},
                {maxInt + 1, mongo::NumberDouble},
                {minEncodableDouble, mongo::NumberDouble},
                {maxEncodableDouble, mongo::NumberDouble},
                {minEncodableDouble - 1, mongo::NumberLong},
                {maxEncodableDouble + 1, mongo::NumberLong},
                {minDouble, mongo::NumberLong},
                {maxDouble, mongo::NumberLong},
                {minDouble - 1, mongo::NumberLong},
                {maxDouble + 1, mongo::NumberLong},
                {minLongLong, mongo::NumberLong},
                {maxLongLong, mongo::NumberLong},
                {0, mongo::Undefined}};
    for (int i = 0; data[i].t != mongo::Undefined; i++) {
        long long v = data[i].v;
        BSONObjBuilder b;
        b.appendNumber("a", v);
        BSONObj o = b.obj();
        ASSERT_EQUALS(o.nFields(), 1);
        BSONElement e = o.getField("a");
        if (data[i].t != mongo::NumberDouble) {
            long long n = e.numberLong();
            ASSERT_EQUALS(n, v);
        } else {
            double n = e.numberDouble();
            ASSERT_APPROX_EQUAL(n, static_cast<double>(v), 0.001);
        }
        assertBSONTypeEquals(e.type(), data[i].t, v, i);
    }
}

TEST(BSONObjBuilderTest, StreamLongLongMin) {
    BSONObj o = BSON("a" << std::numeric_limits<long long>::min());
    ASSERT_EQUALS(o.nFields(), 1);
    BSONElement e = o.getField("a");
    long long n = e.numberLong();
    ASSERT_EQUALS(n, std::numeric_limits<long long>::min());
}

TEST(BSONObjBuilderTest, AppendNumberLongLongMinCompareObject) {
    BSONObjBuilder b;
    b.appendNumber("a", std::numeric_limits<long long>::min());
    BSONObj o1 = b.obj();

    BSONObj o2 = BSON("a" << std::numeric_limits<long long>::min());

    ASSERT_EQUALS(o1, o2);
}

TEST(BSONObjBuilderTest, AppendMaxTimestampConversion) {
    BSONObjBuilder b;
    b.appendMaxForType("a", mongo::bsonTimestamp);
    BSONObj o1 = b.obj();

    BSONElement e = o1.getField("a");
    ASSERT_FALSE(e.eoo());

    mongo::Timestamp timestamp = e.timestamp();
    ASSERT_FALSE(timestamp.isNull());
}

TEST(BSONObjBuilderTest, ResumeBuilding) {
    BufBuilder b;
    {
        BSONObjBuilder firstBuilder(b);
        firstBuilder.append("a", "b");
    }
    {
        BSONObjBuilder secondBuilder(BSONObjBuilder::ResumeBuildingTag(), b);
        secondBuilder.append("c", "d");
    }
    auto obj = BSONObj(b.buf());
    ASSERT_EQ(obj,
              BSON("a"
                   << "b"
                   << "c"
                   << "d"));
}

TEST(BSONObjBuilderTest, ResumeBuildingWithNesting) {
    BufBuilder b;
    // build a trivial object.
    {
        BSONObjBuilder firstBuilder(b);
        firstBuilder.append("ll",
                            BSON("f" << BSON("cc"
                                             << "dd")));
    }
    // add a complex field
    {
        BSONObjBuilder secondBuilder(BSONObjBuilder::ResumeBuildingTag(), b);
        secondBuilder.append("a", BSON("c" << 3));
    }
    auto obj = BSONObj(b.buf());
    ASSERT_EQ(obj,
              BSON("ll" << BSON("f" << BSON("cc"
                                            << "dd"))
                        << "a"
                        << BSON("c" << 3)));
}

TEST(BSONObjBuilderTest, ResetToEmptyResultsInEmptyObj) {
    BSONObjBuilder bob;
    bob.append("a", 3);
    bob.resetToEmpty();
    ASSERT_EQ(BSONObj(), bob.obj());
}

TEST(BSONObjBuilderTest, ResetToEmptyForNestedBuilderOnlyResetsInnerObj) {
    BSONObjBuilder bob;
    bob.append("a", 3);
    BSONObjBuilder innerObj(bob.subobjStart("nestedObj"));
    innerObj.append("b", 4);
    innerObj.resetToEmpty();
    innerObj.done();
    ASSERT_EQ(BSON("a" << 3 << "nestedObj" << BSONObj()), bob.obj());
}

}  // unnamed namespace
}  // namespace mongo
