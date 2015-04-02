// key_string_test.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include <cmath>

#include "mongo/platform/basic.h"
#include "mongo/config.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/base/owned_pointer_vector.h"

using std::string;
using namespace mongo;

BSONObj toBson(const KeyString& ks, Ordering ord) {
    return KeyString::toBson(ks.getBuffer(), ks.getSize(), ord, ks.getTypeBits());
}

Ordering ALL_ASCENDING = Ordering::make(BSONObj());
Ordering ONE_ASCENDING = Ordering::make(BSON("a" << 1));
Ordering ONE_DESCENDING = Ordering::make(BSON("a" << -1));

TEST(KeyStringTest, Simple1) {
    BSONObj a = BSON("" << 5);
    BSONObj b = BSON("" << 6);

    ASSERT_LESS_THAN(a, b);

    ASSERT_LESS_THAN(KeyString(a, ALL_ASCENDING, RecordId()),
                     KeyString(b, ALL_ASCENDING, RecordId()));
}

#define ROUNDTRIP_ORDER(x, order) do {                           \
        const BSONObj _orig = x;                                 \
        const KeyString _ks(_orig, order);                       \
        const BSONObj _converted = toBson(_ks, order);           \
        ASSERT_EQ(_converted, _orig);                            \
        ASSERT(_converted.binaryEqual(_orig));                   \
    } while (0)

#define ROUNDTRIP(x) do {                                            \
        ROUNDTRIP_ORDER(x, ALL_ASCENDING);                           \
        ROUNDTRIP_ORDER(x, ONE_DESCENDING);                          \
    } while (0)

#define COMPARES_SAME(_x,_y) do {                                \
        KeyString _xKS(_x, ONE_ASCENDING);                       \
        KeyString _yKS(_y, ONE_ASCENDING);                       \
        if (_x == _y) {                                          \
            ASSERT_EQUALS(_xKS, _yKS);                           \
        }                                                        \
        else if (_x < _y) {                                      \
            ASSERT_LESS_THAN(_xKS, _yKS);                        \
        }                                                        \
        else {                                                   \
            ASSERT_LESS_THAN(_yKS, _xKS);                        \
        }                                                        \
                                                                 \
        _xKS.resetToKey(_x, ONE_DESCENDING);                     \
        _yKS.resetToKey(_y, ONE_DESCENDING);                     \
        if (_x == _y) {                                          \
            ASSERT_EQUALS(_xKS, _yKS);                           \
        }                                                        \
        else if (_x < _y) {                                      \
            ASSERT_GREATER_THAN(_xKS, _yKS);                     \
        }                                                        \
        else {                                                   \
            ASSERT_GREATER_THAN(_yKS, _xKS);                     \
        }                                                        \
    } while (0)

TEST(KeyStringTest, ActualBytesDouble) {
    // just one test like this for utter sanity

    BSONObj a = BSON("" << 5.5 );
    KeyString ks(a, ALL_ASCENDING);
    log() << "size: " << ks.getSize() << " hex [" << toHex(ks.getBuffer(), ks.getSize()) << "]";

    ASSERT_EQUALS(10U, ks.getSize());

    string hex = "2B" // kNumericPositive1ByteInt
        "0B" // (5 << 1) | 1
        "02000000000000" // fractional bytes of double
        "04"; // kEnd

    ASSERT_EQUALS(hex,
                  toHex(ks.getBuffer(), ks.getSize()));

    ks.resetToKey(a, Ordering::make(BSON("a" << -1)));

    ASSERT_EQUALS(10U, ks.getSize());


    // last byte (kEnd) doesn't get flipped
    string hexFlipped;
    for ( size_t i = 0; i < hex.size()-2; i += 2 ) {
        char c = fromHex(hex.c_str() + i);
        c = ~c;
        hexFlipped += toHex(&c, 1);
    }
    hexFlipped += hex.substr(hex.size()-2);

    ASSERT_EQUALS(hexFlipped,
                  toHex(ks.getBuffer(), ks.getSize()));
}

TEST(KeyStringTest, AllTypesSimple) {
    ROUNDTRIP(BSON("" << 5.5));
    ROUNDTRIP(BSON("" << "abc"));
    ROUNDTRIP(BSON("" << BSON("a" << 5)));
    ROUNDTRIP(BSON("" << BSON_ARRAY("a" << 5)));
    ROUNDTRIP(BSON("" << BSONBinData( "abc", 3, bdtCustom )));
    ROUNDTRIP(BSON("" << BSONUndefined));
    ROUNDTRIP(BSON("" << OID("abcdefabcdefabcdefabcdef")));
    ROUNDTRIP(BSON("" << true));
    ROUNDTRIP(BSON("" << Date_t(123123123)));
    ROUNDTRIP(BSON("" << BSONRegEx("asdf", "x")));
    ROUNDTRIP(BSON("" << BSONDBRef("db.c", OID("010203040506070809101112"))));
    ROUNDTRIP(BSON("" << BSONCode("abc_code")));
    ROUNDTRIP(BSON("" << BSONCodeWScope("def_code", BSON("x_scope" << "a"))));
    ROUNDTRIP(BSON("" << 5));
    ROUNDTRIP(BSON("" << OpTime(123123, 123)));
    ROUNDTRIP(BSON("" << 1235123123123LL));
}

TEST(KeyStringTest, Array1) {
    BSONObj emptyArray = BSON("" << BSONArray());

    ASSERT_EQUALS(Array, emptyArray.firstElement().type());

    ROUNDTRIP(emptyArray);
    ROUNDTRIP(BSON("" << BSON_ARRAY(emptyArray.firstElement())));
    ROUNDTRIP(BSON("" << BSON_ARRAY(1)));
    ROUNDTRIP(BSON("" << BSON_ARRAY(1 << 2)));
    ROUNDTRIP(BSON("" << BSON_ARRAY(1 << 2 << 3)));

    {
        KeyString a(emptyArray, ALL_ASCENDING, RecordId::min());
        KeyString b(emptyArray, ALL_ASCENDING, RecordId(5));
        ASSERT_LESS_THAN(a, b);
    }

    {
        KeyString a(emptyArray, ALL_ASCENDING, RecordId(0));
        KeyString b(emptyArray, ALL_ASCENDING, RecordId(5));
        ASSERT_LESS_THAN(a, b);
    }

}

TEST(KeyStringTest, SubDoc1) {
    ROUNDTRIP(BSON("" << BSON("foo" << 2)));
    ROUNDTRIP(BSON("" << BSON("foo" << 2 << "bar" << "asd")));
    ROUNDTRIP(BSON("" << BSON("foo" << BSON_ARRAY(2 << 4))));
}

TEST(KeyStringTest, SubDoc2) {
    BSONObj a = BSON("" << BSON("a" << "foo"));
    BSONObj b = BSON("" << BSON("b" << 5.5));
    BSONObj c = BSON("" << BSON("c" << BSON("x" << 5)));
    ROUNDTRIP(a);
    ROUNDTRIP(b);
    ROUNDTRIP(c);

    COMPARES_SAME(a,b);
    COMPARES_SAME(a,c);
    COMPARES_SAME(b,c);
}


TEST(KeyStringTest, Compound1) {
    ROUNDTRIP(BSON("" << BSON("a" << 5) << "" << 1));
    ROUNDTRIP(BSON("" << BSON("" << 5) << "" << 1));
}

TEST(KeyStringTest, Undef1) {
    ROUNDTRIP(BSON("" << BSONUndefined));
}

TEST(KeyStringTest, NumberLong0) {
    double d = (1ll << 52) - 1;
    long long ll = static_cast<long long>(d);
    double d2 = static_cast<double>(ll);
    ASSERT_EQUALS(d, d2);
}

TEST(KeyStringTest, NumbersNearInt32Max) {
    int64_t start = std::numeric_limits<int32_t>::max();
    for (int64_t i = -1000; i < 1000; i++) {
        long long toTest = start + i;
        ROUNDTRIP(BSON("" << toTest));
        ROUNDTRIP(BSON("" << static_cast<int>(toTest)));
        ROUNDTRIP(BSON("" << static_cast<double>(toTest)));
    }
}

TEST(KeyStringTest, LotsOfNumbers1) {
    for (int i = 0; i < 64; i++) {
        int64_t x = 1LL << i;
        ROUNDTRIP(BSON("" << static_cast<long long>(x)));
        ROUNDTRIP(BSON("" << static_cast<int>(x)));
        ROUNDTRIP(BSON("" << static_cast<double>(x)));
        ROUNDTRIP(BSON("" << (static_cast<double>(x) + .1)));
        ROUNDTRIP(BSON("" << (static_cast<double>(x) - .1)));

        ROUNDTRIP(BSON("" << (static_cast<long long>(x) + 1)));
        ROUNDTRIP(BSON("" << (static_cast<int>(x) + 1)));
        ROUNDTRIP(BSON("" << (static_cast<double>(x) + 1)));
        ROUNDTRIP(BSON("" << (static_cast<double>(x) + 1.1)));

        ROUNDTRIP(BSON("" << -static_cast<long long>(x)));
        ROUNDTRIP(BSON("" << -static_cast<int>(x)));
        ROUNDTRIP(BSON("" << -static_cast<double>(x)));
        ROUNDTRIP(BSON("" << -(static_cast<double>(x) + .1)));

        ROUNDTRIP(BSON("" << -(static_cast<long long>(x) + 1)));
        ROUNDTRIP(BSON("" << -(static_cast<int>(x) + 1)));
        ROUNDTRIP(BSON("" << -(static_cast<double>(x) + 1)));
        ROUNDTRIP(BSON("" << -(static_cast<double>(x) + 1.1)));

    }
}

TEST(KeyStringTest, LotsOfNumbers2) {
    for (double i = -1100; i < 1100; i++) {
        double x = pow(2, i);
        ROUNDTRIP(BSON("" << x));
    }
    for (double i = -1100; i < 1100; i++) {
        double x = pow(2.1, i);
        ROUNDTRIP(BSON("" << x));
    }
}

TEST(KeyStringTest, RecordIdOrder1) {

    Ordering ordering = Ordering::make(BSON("a" << 1));

    KeyString a(BSON("" << 5), ordering, RecordId::min());
    KeyString b(BSON("" << 5), ordering, RecordId(2));
    KeyString c(BSON("" << 5), ordering, RecordId(3));
    KeyString d(BSON("" << 6), ordering, RecordId());
    KeyString e(BSON("" << 6), ordering, RecordId(1));

    ASSERT_LESS_THAN(a, b);
    ASSERT_LESS_THAN(b, c);
    ASSERT_LESS_THAN(c, d);
    ASSERT_LESS_THAN(d, e);

}

TEST(KeyStringTest, RecordIdOrder2) {

    Ordering ordering = Ordering::make(BSON("a" << -1 << "b" << -1));

    KeyString a(BSON("" << 5 << "" << 6), ordering, RecordId::min());
    KeyString b(BSON("" << 5 << "" << 6), ordering, RecordId(5));
    KeyString c(BSON("" << 5 << "" << 5), ordering, RecordId(4));
    KeyString d(BSON("" << 3 << "" << 4), ordering, RecordId(3));

    ASSERT_LESS_THAN(a, b);
    ASSERT_LESS_THAN(b, c);
    ASSERT_LESS_THAN(c, d);
    ASSERT_LESS_THAN(a, c);
    ASSERT_LESS_THAN(a, d);
    ASSERT_LESS_THAN(b, d);
}

TEST(KeyStringTest, RecordIdOrder2Double) {

    Ordering ordering = Ordering::make(BSON("a" << -1 << "b" << -1));

    KeyString a(BSON("" << 5.0 << "" << 6.0), ordering, RecordId::min());
    KeyString b(BSON("" << 5.0 << "" << 6.0), ordering, RecordId(5));
    KeyString c(BSON("" << 3.0 << "" << 4.0), ordering, RecordId(3));

    ASSERT_LESS_THAN(a, b);
    ASSERT_LESS_THAN(b, c);
    ASSERT_LESS_THAN(a, c);
}

TEST(KeyStringTest, OpTime) {

    BSONObj a = BSON("" << OpTime(0, 0));
    BSONObj b = BSON("" << OpTime(1234, 1));
    BSONObj c = BSON("" << OpTime(1234, 2));
    BSONObj d = BSON("" << OpTime(1235, 1));

    {
        ROUNDTRIP(a);
        ROUNDTRIP(b);
        ROUNDTRIP(c);

        ASSERT_LESS_THAN(a, b);
        ASSERT_LESS_THAN(b, c);
        ASSERT_LESS_THAN(c, d);

        KeyString ka(a, ALL_ASCENDING);
        KeyString kb(b, ALL_ASCENDING);
        KeyString kc(c, ALL_ASCENDING);
        KeyString kd(d, ALL_ASCENDING);

        ASSERT(ka.compare(kb) < 0);
        ASSERT(kb.compare(kc) < 0);
        ASSERT(kc.compare(kd) < 0);
    }

    {
        Ordering ALL_ASCENDING = Ordering::make(BSON("a" << -1));

        ROUNDTRIP(a);
        ROUNDTRIP(b);
        ROUNDTRIP(c);

        ASSERT(d.woCompare(c, ALL_ASCENDING) < 0);
        ASSERT(c.woCompare(b, ALL_ASCENDING) < 0);
        ASSERT(b.woCompare(a, ALL_ASCENDING) < 0);

        KeyString ka(a, ALL_ASCENDING);
        KeyString kb(b, ALL_ASCENDING);
        KeyString kc(c, ALL_ASCENDING);
        KeyString kd(d, ALL_ASCENDING);

        ASSERT(ka.compare(kb) > 0);
        ASSERT(kb.compare(kc) > 0);
        ASSERT(kc.compare(kd) > 0);
    }

}

TEST(KeyStringTest, AllTypesRoundtrip) {
    for ( int i = 1; i <= JSTypeMax; i++ ) {
        {
            BSONObjBuilder b;
            b.appendMinForType("", i );
            BSONObj o = b.obj();
            ROUNDTRIP(o);
        }
        {
            BSONObjBuilder b;
            b.appendMaxForType("", i );
            BSONObj o = b.obj();
            ROUNDTRIP(o);
        }
    }
}

const std::vector<BSONObj>& getInterestingElements() {
    static std::vector<BSONObj> elements;

    if (!elements.empty()) {
        return elements;
    }
    
    // These are used to test strings that include NUL bytes.
    const StringData ball("ball", StringData::LiteralTag());
    const StringData ball00n("ball\0\0n", StringData::LiteralTag());

    elements.push_back(BSON("" << 1));
    elements.push_back(BSON("" << 1.0));
    elements.push_back(BSON("" << 1LL));
    elements.push_back(BSON("" << 123456789123456789LL));
    elements.push_back(BSON("" << -123456789123456789LL));
    elements.push_back(BSON("" << 112353998331165715LL));
    elements.push_back(BSON("" << 112353998331165710LL));
    elements.push_back(BSON("" << 1123539983311657199LL));
    elements.push_back(BSON("" << 123456789123456789.123));
    elements.push_back(BSON("" << -123456789123456789.123));
    elements.push_back(BSON("" << 112353998331165715.0));
    elements.push_back(BSON("" << 112353998331165710.0));
    elements.push_back(BSON("" << 1123539983311657199.0));
    elements.push_back(BSON("" << 5.0));
    elements.push_back(BSON("" << 5));
    elements.push_back(BSON("" << 2));
    elements.push_back(BSON("" << -2));
    elements.push_back(BSON("" << -2.2));
    elements.push_back(BSON("" << -12312312.2123123123123));
    elements.push_back(BSON("" << 12312312.2123123123123));
    elements.push_back(BSON("" << "aaa"));
    elements.push_back(BSON("" << "AAA"));
    elements.push_back(BSON("" << ball));
    elements.push_back(BSON("" << ball00n));
    elements.push_back(BSON("" << BSONSymbol(ball)));
    elements.push_back(BSON("" << BSONSymbol(ball00n)));
    elements.push_back(BSON("" << BSON("a" << 5)));
    elements.push_back(BSON("" << BSON("a" << 6)));
    elements.push_back(BSON("" << BSON("b" << 6)));
    elements.push_back(BSON("" << BSON_ARRAY("a" << 5)));
    elements.push_back(BSON("" << BSONNULL));
    elements.push_back(BSON("" << BSONUndefined));
    elements.push_back(BSON("" << OID("abcdefabcdefabcdefabcdef")));
    elements.push_back(BSON("" << Date_t(123)));
    elements.push_back(BSON("" << BSONCode("abc_code")));
    elements.push_back(BSON("" << BSONCode(ball)));
    elements.push_back(BSON("" << BSONCode(ball00n)));
    elements.push_back(BSON("" << BSONCodeWScope("def_code1", BSON("x_scope" << "a"))));
    elements.push_back(BSON("" << BSONCodeWScope("def_code2", BSON("x_scope" << "a"))));
    elements.push_back(BSON("" << BSONCodeWScope("def_code2", BSON("x_scope" << "b"))));
    elements.push_back(BSON("" << BSONCodeWScope(ball, BSON("a" << 1))));
    elements.push_back(BSON("" << BSONCodeWScope(ball00n, BSON("a" << 1))));
    elements.push_back(BSON("" << true));
    elements.push_back(BSON("" << false));

    // Something that needs multiple bytes of typeBits
    elements.push_back(BSON("" << BSON_ARRAY(""
                                          << BSONSymbol("")
                                          << 0
                                          << 0ll
                                          << 0.0
                                          << -0.0
                                          )));

    //
    // Interesting numeric cases
    //

    elements.push_back(BSON("" << 0));
    elements.push_back(BSON("" << 0ll));
    elements.push_back(BSON("" << 0.0));
    elements.push_back(BSON("" << -0.0));
    elements.push_back(BSON("" << std::numeric_limits<double>::quiet_NaN()));
    elements.push_back(BSON("" << std::numeric_limits<double>::infinity()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::infinity()));
    elements.push_back(BSON("" << std::numeric_limits<double>::max()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::max()));
    elements.push_back(BSON("" << std::numeric_limits<double>::min()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::min()));
    elements.push_back(BSON("" << std::numeric_limits<double>::denorm_min()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::denorm_min()));
    elements.push_back(BSON("" << std::numeric_limits<double>::denorm_min()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::denorm_min()));

    elements.push_back(BSON("" << std::numeric_limits<long long>::max()));
    elements.push_back(BSON("" << -std::numeric_limits<long long>::max()));
    elements.push_back(BSON("" << std::numeric_limits<long long>::min()));

    elements.push_back(BSON("" << std::numeric_limits<int>::max()));
    elements.push_back(BSON("" << -std::numeric_limits<int>::max()));
    elements.push_back(BSON("" << std::numeric_limits<int>::min()));

    for (int powerOfTwo = 0; powerOfTwo < 63; powerOfTwo++) {
        const long long lNum = 1ll << powerOfTwo;
        const double dNum = double(lNum);

        // All powers of two in this range can be represented exactly as doubles.
        invariant(lNum == static_cast<long long>(dNum));

        elements.push_back(BSON("" << lNum));
        elements.push_back(BSON("" << -lNum));

        elements.push_back(BSON("" << dNum));
        elements.push_back(BSON("" << -dNum));


        elements.push_back(BSON("" << (lNum + 1)));
        elements.push_back(BSON("" << (lNum - 1)));
        elements.push_back(BSON("" << (-lNum + 1)));
        elements.push_back(BSON("" << (-lNum - 1)));

        if (powerOfTwo <= 52) { // is dNum - 0.5 representable?
            elements.push_back(BSON("" << (dNum - 0.5)));
            elements.push_back(BSON("" << -(dNum - 0.5)));
        }

        if (powerOfTwo <= 51) { // is dNum + 0.5 representable?
            elements.push_back(BSON("" << (dNum + 0.5)));
            elements.push_back(BSON("" << -(dNum + 0.5)));
        }
    }

    {
        // Numbers around +/- numeric_limits<long long>::max() which can't be represented
        // precisely as a double.
        const long long maxLL = std::numeric_limits<long long>::max();
        const double closestAbove = 9223372036854775808.0; // 2**63
        const double closestBelow = 9223372036854774784.0; // 2**63 - epsilon

        elements.push_back(BSON("" << maxLL));
        elements.push_back(BSON("" << (maxLL - 1)));
        elements.push_back(BSON("" << closestAbove));
        elements.push_back(BSON("" << closestBelow));

        elements.push_back(BSON("" << -maxLL));
        elements.push_back(BSON("" << -(maxLL - 1)));
        elements.push_back(BSON("" << -closestAbove));
        elements.push_back(BSON("" << -closestBelow));
    }

    {
        // Numbers around numeric_limits<long long>::min() which can be represented precisely as
        // a double, but not as a positive long long.
        const long long minLL = std::numeric_limits<long long>::min();
        const double closestBelow = -9223372036854777856.0; // -2**63 - epsilon
        const double equal = -9223372036854775808.0; // 2**63
        const double closestAbove = -9223372036854774784.0; // -2**63 + epsilon

        elements.push_back(BSON("" << minLL));
        elements.push_back(BSON("" << equal));
        elements.push_back(BSON("" << closestAbove));
        elements.push_back(BSON("" << closestBelow));
    }

    return elements;
}

void testPermutation(const std::vector<BSONObj>& elementsOrig,
                     const std::vector<BSONObj>& orderings,
                     bool debug) {

    // Since KeyStrings are compared using memcmp we can assume it provides a total ordering such
    // that there won't be cases where (a < b && b < c && !(a < c)). This test still needs to ensure
    // that it provides the *correct* total ordering.
    for (size_t k = 0; k < orderings.size(); k++) {
        BSONObj orderObj = orderings[k];
        Ordering ordering = Ordering::make(orderObj);
        if (debug) log() << "ordering: " << orderObj;

        std::vector<BSONObj> elements = elementsOrig;
        std::stable_sort(elements.begin(), elements.end(), BSONObjCmp(orderObj));

        for (size_t i = 0; i < elements.size(); i++) {
            const BSONObj& o1 = elements[i];
            if (debug) log() << "\to1: " << o1;
            ROUNDTRIP_ORDER(o1, ordering);

            KeyString k1(o1, ordering);

            KeyString l1(BSON("l" << o1.firstElement()), ordering); // kLess
            KeyString g1(BSON("g" << o1.firstElement()), ordering); // kGreater
            ASSERT_LT(l1, k1);
            ASSERT_GT(g1, k1);

            if (i + 1 < elements.size()) {
                const BSONObj& o2 = elements[i + 1];
                if (debug) log() << "\t\t o2: " << o2;
                KeyString k2(o2, ordering);
                KeyString g2(BSON("g" << o2.firstElement()), ordering);
                KeyString l2(BSON("l" << o2.firstElement()), ordering);

                int bsonCmp = o1.woCompare(o2, ordering);
                invariant(bsonCmp <= 0); // We should be sorted...

                if (bsonCmp == 0) {
                    ASSERT_EQ(k1, k2);
                }
                else {
                    ASSERT_LT(k1, k2);
                }

                // Test the query encodings using kLess and kGreater
                int firstElementComp = o1.firstElement().woCompare(o2.firstElement());
                if (ordering.descending(1))
                    firstElementComp = -firstElementComp;

                invariant(firstElementComp <= 0);

                if (firstElementComp == 0) {
                    // If they share a first element then l1/g1 should equal l2/g2 and l1 should be
                    // less than both and g1 should be greater than both.
                    ASSERT_EQ(l1, l2);
                    ASSERT_EQ(g1, g2);
                    ASSERT_LT(l1, k2);
                    ASSERT_GT(g1, k2);
                }
                else {
                    // k1 is less than k2. Less(k2) and Greater(k1) should be between them.
                    ASSERT_LT(g1, k2);
                    ASSERT_GT(l2, k1);
                }
            }
        }
    }
}

TEST(KeyStringTest, AllPermCompare) {
    const std::vector<BSONObj>& elements = getInterestingElements();

    for (size_t i = 0; i < elements.size(); i++) {
        const BSONObj& o = elements[i];
        ROUNDTRIP(o);
    }

    std::vector<BSONObj> orderings;
    orderings.push_back(BSON("a" << 1));
    orderings.push_back(BSON("a" << -1));

    testPermutation(elements, orderings, false);
}

TEST(KeyStringTest, AllPerm2Compare) {
    // This test can take over a minute without optimizations. Re-enable if you need to debug it.
#if !defined(MONGO_CONFIG_OPTIMIZED_BUILD)
    log() << "\t\t\tskipping test on non-optimized build";
    return;
#endif

    const std::vector<BSONObj>& baseElements = getInterestingElements();

    std::vector<BSONObj> elements;
    for (size_t i = 0; i < baseElements.size(); i++) {
        for (size_t j = 0; j < baseElements.size(); j++) {
            BSONObjBuilder b;
            b.appendElements(baseElements[i]);
            b.appendElements(baseElements[j]);
            BSONObj o = b.obj();
            elements.push_back(o);
        }
    }

    log() << "AllPrem2Compare size:" << elements.size();

    for (size_t i = 0; i < elements.size(); i++) {
        const BSONObj& o = elements[i];
        ROUNDTRIP(o);
    }

    std::vector<BSONObj> orderings;
    orderings.push_back(BSON("a" << 1 << "b" << 1));
    orderings.push_back(BSON("a" << -1 << "b" << 1));
    orderings.push_back(BSON("a" << 1 << "b" << -1));
    orderings.push_back(BSON("a" << -1 << "b" << -1));

    testPermutation(elements, orderings, false);
}

#define COMPARE_HELPER(LHS, RHS)                        \
    (((LHS) < (RHS)) ? -1 : (((LHS) == (RHS)) ? 0 : 1))

int compareLongToDouble(long long lhs, double rhs) {
    if (rhs >= std::numeric_limits<long long>::max())
        return -1;
    if (rhs < std::numeric_limits<long long>::min() )
        return 1;

    if (fabs(rhs) >= (1LL << 52)) {
        return COMPARE_HELPER(lhs, static_cast<long long>(rhs));
    }

    return COMPARE_HELPER(static_cast<double>(lhs), rhs);
}

int compareNumbers(const BSONElement& lhs, const BSONElement& rhs ) {
    invariant(lhs.isNumber());
    invariant(rhs.isNumber());

    if (lhs.type() == NumberInt || lhs.type() == NumberLong) {
        if (rhs.type() == NumberInt || rhs.type() == NumberLong) {
            return COMPARE_HELPER(lhs.numberLong(), rhs.numberLong());
        }
        return compareLongToDouble(lhs.numberLong(), rhs.Double());
    }
    else { // double
        if (rhs.type() == NumberDouble) {
            return COMPARE_HELPER(lhs.Double(), rhs.Double());
        }
        return -compareLongToDouble(rhs.numberLong(), lhs.Double());
    }
}

TEST(KeyStringTest, NaNs) {
    // TODO use hex floats to force distinct NaNs
    const double nan1 = std::numeric_limits<double>::quiet_NaN();
    const double nan2 = std::numeric_limits<double>::signaling_NaN();

    // Since only output a single NaN, we can't use the normal ROUNDTRIP testing here.

    const KeyString ks1a(BSON("" << nan1), ONE_ASCENDING);
    const KeyString ks1d(BSON("" << nan1), ONE_DESCENDING);

    const KeyString ks2a(BSON("" << nan2), ONE_ASCENDING);
    const KeyString ks2d(BSON("" << nan2), ONE_DESCENDING);

    ASSERT_EQ(ks1a, ks2a);
    ASSERT_EQ(ks1d, ks2d);

    ASSERT(std::isnan(toBson(ks1a, ONE_ASCENDING)[""].Double()));
    ASSERT(std::isnan(toBson(ks2a, ONE_ASCENDING)[""].Double()));
    ASSERT(std::isnan(toBson(ks1d, ONE_DESCENDING)[""].Double()));
    ASSERT(std::isnan(toBson(ks2d, ONE_DESCENDING)[""].Double()));
}
TEST(KeyStringTest, NumberOrderLots) {
    std::vector<BSONObj> numbers;
    {
        numbers.push_back(BSON("" << 0));
        numbers.push_back(BSON("" << 0.0));
        numbers.push_back(BSON("" << -0.0));

        numbers.push_back(BSON("" << std::numeric_limits<long long>::min()));
        numbers.push_back(BSON("" << std::numeric_limits<long long>::max()));
        numbers.push_back(BSON("" << static_cast<double>(std::numeric_limits<long long>::min())));
        numbers.push_back(BSON("" << static_cast<double>(std::numeric_limits<long long>::max())));
        numbers.push_back(BSON("" << std::numeric_limits<double>::min()));
        numbers.push_back(BSON("" << std::numeric_limits<double>::max()));
        numbers.push_back(BSON("" << std::numeric_limits<int>::min()));
        numbers.push_back(BSON("" << std::numeric_limits<int>::max()));
        numbers.push_back(BSON("" << std::numeric_limits<short>::min()));
        numbers.push_back(BSON("" << std::numeric_limits<short>::max()));

        for (int i = 0; i < 64; i++) {
            int64_t x = 1LL << i;
            numbers.push_back(BSON("" << static_cast<long long>(x)));
            numbers.push_back(BSON("" << static_cast<int>(x)));
            numbers.push_back(BSON("" << static_cast<double>(x)));
            numbers.push_back(BSON("" << (static_cast<double>(x) + .1)));

            numbers.push_back(BSON("" << (static_cast<long long>(x) + 1)));
            numbers.push_back(BSON("" << (static_cast<int>(x) + 1)));
            numbers.push_back(BSON("" << (static_cast<double>(x) + 1)));
            numbers.push_back(BSON("" << (static_cast<double>(x) + 1.1)));

            numbers.push_back(BSON("" << -static_cast<long long>(x)));
            numbers.push_back(BSON("" << -static_cast<int>(x)));
            numbers.push_back(BSON("" << -static_cast<double>(x)));
            numbers.push_back(BSON("" << -(static_cast<double>(x) + .1)));

            numbers.push_back(BSON("" << -(static_cast<long long>(x) + 1)));
            numbers.push_back(BSON("" << -(static_cast<int>(x) + 1)));
            numbers.push_back(BSON("" << -(static_cast<double>(x) + 1)));
            numbers.push_back(BSON("" << -(static_cast<double>(x) + 1.1)));
        }

        for (double i = 0; i < 1000; i++) {
            double x = pow(2.1, i);
            numbers.push_back(BSON("" << x));
        }
    }

    Ordering ordering = Ordering::make(BSON("a" << 1));

    OwnedPointerVector<KeyString> keyStrings;
    for (size_t i = 0; i < numbers.size(); i++) {
        keyStrings.push_back(new KeyString(numbers[i], ordering));
    }

    for (size_t i = 0; i < numbers.size(); i++) {
        for (size_t j = 0; j < numbers.size(); j++) {
            const KeyString& a = *keyStrings[i];
            const KeyString& b = *keyStrings[j];
            ASSERT_EQUALS(a.compare(b), -b.compare(a));

            if (a.compare(b) != compareNumbers(numbers[i].firstElement(),
                                               numbers[j].firstElement())) {
                log() << numbers[i] << " " << numbers[j];
            }

            ASSERT_EQUALS(a.compare(b),
                          compareNumbers(numbers[i].firstElement(),
                                         numbers[j].firstElement()));

        }
    }
}

TEST(KeyStringTest, RecordIds) {
    for (int i = 0; i < 63; i++) {
        const RecordId rid = RecordId(1ll << i);

        { // Test encoding / decoding of single RecordIds
            const KeyString ks(rid);
            ASSERT_GTE(ks.getSize(), 2u);
            ASSERT_LTE(ks.getSize(), 10u);

            ASSERT_EQ(KeyString::decodeRecordIdAtEnd(ks.getBuffer(), ks.getSize()), rid);

            {
                BufReader reader(ks.getBuffer(), ks.getSize());
                ASSERT_EQ(KeyString::decodeRecordId(&reader), rid);
                ASSERT(reader.atEof());
            }

            if (rid.isNormal()) {
                ASSERT_GT(ks, KeyString(RecordId()));
                ASSERT_GT(ks, KeyString(RecordId::min()));
                ASSERT_LT(ks, KeyString(RecordId::max()));

                ASSERT_GT(ks, KeyString(RecordId(rid.repr() - 1)));
                ASSERT_LT(ks, KeyString(RecordId(rid.repr() + 1)));
            }
        }

        for (int j = 0; j < 63; j++) {
            RecordId other = RecordId(1ll << j);

            if (rid == other) ASSERT_EQ(KeyString(rid), KeyString(other));
            if (rid < other) ASSERT_LT(KeyString(rid), KeyString(other));
            if (rid > other) ASSERT_GT(KeyString(rid), KeyString(other));

            {
                // Test concatenating RecordIds like in a unique index.
                KeyString ks;
                ks.appendRecordId(RecordId::max()); // uses all bytes
                ks.appendRecordId(rid);
                ks.appendRecordId(RecordId(0xDEADBEEF)); // uses some extra bytes
                ks.appendRecordId(rid);
                ks.appendRecordId(RecordId(1)); // uses no extra bytes
                ks.appendRecordId(rid);
                ks.appendRecordId(other);

                ASSERT_EQ(KeyString::decodeRecordIdAtEnd(ks.getBuffer(), ks.getSize()), other);

                // forward scan
                BufReader reader(ks.getBuffer(), ks.getSize());
                ASSERT_EQ(KeyString::decodeRecordId(&reader), RecordId::max());
                ASSERT_EQ(KeyString::decodeRecordId(&reader), rid);
                ASSERT_EQ(KeyString::decodeRecordId(&reader), RecordId(0xDEADBEEF));
                ASSERT_EQ(KeyString::decodeRecordId(&reader), rid);
                ASSERT_EQ(KeyString::decodeRecordId(&reader), RecordId(1));
                ASSERT_EQ(KeyString::decodeRecordId(&reader), rid);
                ASSERT_EQ(KeyString::decodeRecordId(&reader), other);
                ASSERT(reader.atEof());
            }
        }
    }
}

