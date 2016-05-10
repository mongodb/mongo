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

#include "mongo/platform/basic.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <typeinfo>
#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/config.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

using std::string;
using namespace mongo;

BSONObj toBson(const KeyString& ks, Ordering ord) {
    return KeyString::toBson(ks.getBuffer(), ks.getSize(), ord, ks.getTypeBits());
}

Ordering ALL_ASCENDING = Ordering::make(BSONObj());
Ordering ONE_ASCENDING = Ordering::make(BSON("a" << 1));
Ordering ONE_DESCENDING = Ordering::make(BSON("a" << -1));

class KeyStringTest : public mongo::unittest::Test {
public:
    void run() {
        auto base = static_cast<mongo::unittest::Test*>(this);
        try {
            version = KeyString::Version::V0;
            base->run();
            version = KeyString::Version::V1;
            base->run();
        } catch (...) {
            log() << "exception while testing KeyString version "
                  << mongo::KeyString::versionToString(version);
            throw;
        }
    }

protected:
    KeyString::Version version;
};


TEST_F(KeyStringTest, Simple1) {
    BSONObj a = BSON("" << 5);
    BSONObj b = BSON("" << 6);

    ASSERT_LESS_THAN(a, b);

    ASSERT_LESS_THAN(KeyString(version, a, ALL_ASCENDING, RecordId()),
                     KeyString(version, b, ALL_ASCENDING, RecordId()));
}

#define ROUNDTRIP_ORDER(version, x, order)             \
    do {                                               \
        const BSONObj _orig = x;                       \
        const KeyString _ks(version, _orig, order);    \
        const BSONObj _converted = toBson(_ks, order); \
        ASSERT_EQ(_converted, _orig);                  \
        ASSERT(_converted.binaryEqual(_orig));         \
    } while (0)

#define ROUNDTRIP(version, x)                        \
    do {                                             \
        ROUNDTRIP_ORDER(version, x, ALL_ASCENDING);  \
        ROUNDTRIP_ORDER(version, x, ONE_DESCENDING); \
    } while (0)

#define COMPARES_SAME(_v, _x, _y)              \
    do {                                       \
        KeyString _xKS(_v, _x, ONE_ASCENDING); \
        KeyString _yKS(_v, _y, ONE_ASCENDING); \
        if (_x == _y) {                        \
            ASSERT_EQUALS(_xKS, _yKS);         \
        } else if (_x < _y) {                  \
            ASSERT_LESS_THAN(_xKS, _yKS);      \
        } else {                               \
            ASSERT_LESS_THAN(_yKS, _xKS);      \
        }                                      \
                                               \
        _xKS.resetToKey(_x, ONE_DESCENDING);   \
        _yKS.resetToKey(_y, ONE_DESCENDING);   \
        if (_x == _y) {                        \
            ASSERT_EQUALS(_xKS, _yKS);         \
        } else if (_x < _y) {                  \
            ASSERT_GREATER_THAN(_xKS, _yKS);   \
        } else {                               \
            ASSERT_GREATER_THAN(_yKS, _xKS);   \
        }                                      \
    } while (0)

TEST_F(KeyStringTest, ActualBytesDouble) {
    // just one test like this for utter sanity

    BSONObj a = BSON("" << 5.5);
    KeyString ks(version, a, ALL_ASCENDING);
    log() << KeyString::versionToString(version) << " size: " << ks.getSize() << " hex ["
          << toHex(ks.getBuffer(), ks.getSize()) << "]";

    ASSERT_EQUALS(10U, ks.getSize());

    string hex = version == KeyString::Version::V0 ? "2B"              // kNumericPositive1ByteInt
                                                     "0B"              // (5 << 1) | 1
                                                     "02000000000000"  // fractional bytes of double
                                                     "04"              // kEnd
                                                   : "2B"              // kNumericPositive1ByteInt
                                                     "0B"              // (5 << 1) | 1
                                                     "80000000000000"  // fractional bytes
                                                     "04";             // kEnd

    ASSERT_EQUALS(hex, toHex(ks.getBuffer(), ks.getSize()));

    ks.resetToKey(a, Ordering::make(BSON("a" << -1)));

    ASSERT_EQUALS(10U, ks.getSize());


    // last byte (kEnd) doesn't get flipped
    string hexFlipped;
    for (size_t i = 0; i < hex.size() - 2; i += 2) {
        char c = fromHex(hex.c_str() + i);
        c = ~c;
        hexFlipped += toHex(&c, 1);
    }
    hexFlipped += hex.substr(hex.size() - 2);

    ASSERT_EQUALS(hexFlipped, toHex(ks.getBuffer(), ks.getSize()));
}

TEST_F(KeyStringTest, AllTypesSimple) {
    ROUNDTRIP(version, BSON("" << 5.5));
    ROUNDTRIP(version,
              BSON(""
                   << "abc"));
    ROUNDTRIP(version, BSON("" << BSON("a" << 5)));
    ROUNDTRIP(version, BSON("" << BSON_ARRAY("a" << 5)));
    ROUNDTRIP(version, BSON("" << BSONBinData("abc", 3, bdtCustom)));
    ROUNDTRIP(version, BSON("" << BSONUndefined));
    ROUNDTRIP(version, BSON("" << OID("abcdefabcdefabcdefabcdef")));
    ROUNDTRIP(version, BSON("" << true));
    ROUNDTRIP(version, BSON("" << Date_t::fromMillisSinceEpoch(123123123)));
    ROUNDTRIP(version, BSON("" << BSONRegEx("asdf", "x")));
    ROUNDTRIP(version, BSON("" << BSONDBRef("db.c", OID("010203040506070809101112"))));
    ROUNDTRIP(version, BSON("" << BSONCode("abc_code")));
    ROUNDTRIP(version,
              BSON("" << BSONCodeWScope("def_code",
                                        BSON("x_scope"
                                             << "a"))));
    ROUNDTRIP(version, BSON("" << 5));
    ROUNDTRIP(version, BSON("" << Timestamp(123123, 123)));
    ROUNDTRIP(version, BSON("" << Timestamp(~0U, 3)));
    ROUNDTRIP(version, BSON("" << 1235123123123LL));
}

TEST_F(KeyStringTest, Array1) {
    BSONObj emptyArray = BSON("" << BSONArray());

    ASSERT_EQUALS(Array, emptyArray.firstElement().type());

    ROUNDTRIP(version, emptyArray);
    ROUNDTRIP(version, BSON("" << BSON_ARRAY(emptyArray.firstElement())));
    ROUNDTRIP(version, BSON("" << BSON_ARRAY(1)));
    ROUNDTRIP(version, BSON("" << BSON_ARRAY(1 << 2)));
    ROUNDTRIP(version, BSON("" << BSON_ARRAY(1 << 2 << 3)));

    {
        KeyString a(version, emptyArray, ALL_ASCENDING, RecordId::min());
        KeyString b(version, emptyArray, ALL_ASCENDING, RecordId(5));
        ASSERT_LESS_THAN(a, b);
    }

    {
        KeyString a(version, emptyArray, ALL_ASCENDING, RecordId(0));
        KeyString b(version, emptyArray, ALL_ASCENDING, RecordId(5));
        ASSERT_LESS_THAN(a, b);
    }
}

TEST_F(KeyStringTest, SubDoc1) {
    ROUNDTRIP(version, BSON("" << BSON("foo" << 2)));
    ROUNDTRIP(version,
              BSON("" << BSON("foo" << 2 << "bar"
                                    << "asd")));
    ROUNDTRIP(version, BSON("" << BSON("foo" << BSON_ARRAY(2 << 4))));
}

TEST_F(KeyStringTest, SubDoc2) {
    BSONObj a = BSON("" << BSON("a"
                                << "foo"));
    BSONObj b = BSON("" << BSON("b" << 5.5));
    BSONObj c = BSON("" << BSON("c" << BSON("x" << 5)));
    ROUNDTRIP(version, a);
    ROUNDTRIP(version, b);
    ROUNDTRIP(version, c);

    COMPARES_SAME(version, a, b);
    COMPARES_SAME(version, a, c);
    COMPARES_SAME(version, b, c);
}


TEST_F(KeyStringTest, Compound1) {
    ROUNDTRIP(version, BSON("" << BSON("a" << 5) << "" << 1));
    ROUNDTRIP(version, BSON("" << BSON("" << 5) << "" << 1));
}

TEST_F(KeyStringTest, Undef1) {
    ROUNDTRIP(version, BSON("" << BSONUndefined));
}

TEST_F(KeyStringTest, NumberLong0) {
    double d = (1ll << 52) - 1;
    long long ll = static_cast<long long>(d);
    double d2 = static_cast<double>(ll);
    ASSERT_EQUALS(d, d2);
}

TEST_F(KeyStringTest, NumbersNearInt32Max) {
    int64_t start = std::numeric_limits<int32_t>::max();
    for (int64_t i = -1000; i < 1000; i++) {
        long long toTest = start + i;
        ROUNDTRIP(version, BSON("" << toTest));
        ROUNDTRIP(version, BSON("" << static_cast<int>(toTest)));
        ROUNDTRIP(version, BSON("" << static_cast<double>(toTest)));
    }
}

TEST_F(KeyStringTest, DecimalNumbers) {
    if (version == KeyString::Version::V0) {
        log() << "not testing DecimalNumbers for KeyString V0";
        return;
    }

    const auto V1 = KeyString::Version::V1;

    // Zeros
    ROUNDTRIP(V1, BSON("" << Decimal128("0")));
    ROUNDTRIP(V1, BSON("" << Decimal128("0.0")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-0")));
    ROUNDTRIP(V1, BSON("" << Decimal128("0E5000")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-0.0000E-6172")));

    // Special numbers
    ROUNDTRIP(V1, BSON("" << Decimal128("NaN")));
    ROUNDTRIP(V1, BSON("" << Decimal128("+Inf")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-Inf")));

    // Decimal representations of whole double numbers
    ROUNDTRIP(V1, BSON("" << Decimal128("1")));
    ROUNDTRIP(V1, BSON("" << Decimal128("2.0")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-2.0E1")));
    ROUNDTRIP(V1, BSON("" << Decimal128("1234.56E15")));
    ROUNDTRIP(V1, BSON("" << Decimal128("2.00000000000000000000000")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-9223372036854775808.00000000000000")));  // -2**63
    ROUNDTRIP(V1, BSON("" << Decimal128("973555660975280180349468061728768E1")));  // 1.875 * 2**112

    // Decimal representations of fractional double numbers
    ROUNDTRIP(V1, BSON("" << Decimal128("1.25")));
    ROUNDTRIP(V1, BSON("" << Decimal128("3.141592653584666550159454345703125")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-127.50")));

    // Decimal representations of whole int64 non-double numbers
    ROUNDTRIP(V1, BSON("" << Decimal128("243290200817664E4")));  // 20!
    ROUNDTRIP(V1, BSON("" << Decimal128("9007199254740993")));   // 2**53 + 1
    ROUNDTRIP(V1, BSON("" << Decimal128(std::numeric_limits<int64_t>::max())));
    ROUNDTRIP(V1, BSON("" << Decimal128(std::numeric_limits<int64_t>::min())));

    // Decimals in int64_t range without decimal or integer representation
    ROUNDTRIP(V1, BSON("" << Decimal128("1.23")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-1.1")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-12345.60")));
    ROUNDTRIP(V1, BSON("" << Decimal128("3.141592653589793238462643383279502")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-3.141592653589793115997963468544185")));

    // Decimal representations of small double numbers
    ROUNDTRIP(V1, BSON("" << Decimal128("0.50")));
    ROUNDTRIP(V1,
              BSON("" << Decimal128("-0.3552713678800500929355621337890625E-14")));  // -2**(-48)
    ROUNDTRIP(V1,
              BSON("" << Decimal128("-0.000000000000001234567890123456789012345678901234E-99")));

    // Decimal representations of small decimals not representable as double
    ROUNDTRIP(V1, BSON("" << Decimal128("0.02")));

    // Large decimals
    ROUNDTRIP(V1, BSON("" << Decimal128("1234567890123456789012345678901234E6000")));
    ROUNDTRIP(V1,
              BSON("" << Decimal128("-19950631168.80758384883742162683585E3000")));  // -2**10000

    // Tiny, tiny decimals
    ROUNDTRIP(V1,
              BSON("" << Decimal128("0.2512388057698744585180135042133610E-6020")));  // 2**(-10000)
    ROUNDTRIP(V1, BSON("" << Decimal128("4.940656458412465441765687928682213E-324") << "" << 1));
    ROUNDTRIP(V1, BSON("" << Decimal128("-0.8289046058458094980903836776809409E-316")));
}

TEST_F(KeyStringTest, LotsOfNumbers1) {
    for (int i = 0; i < 64; i++) {
        int64_t x = 1LL << i;
        ROUNDTRIP(version, BSON("" << static_cast<long long>(x)));
        ROUNDTRIP(version, BSON("" << static_cast<int>(x)));
        ROUNDTRIP(version, BSON("" << static_cast<double>(x)));
        ROUNDTRIP(version, BSON("" << (static_cast<double>(x) + .1)));
        ROUNDTRIP(version, BSON("" << (static_cast<double>(x) - .1)));

        ROUNDTRIP(version, BSON("" << (static_cast<long long>(x) + 1)));
        ROUNDTRIP(version, BSON("" << (static_cast<int>(x) + 1)));
        ROUNDTRIP(version, BSON("" << (static_cast<double>(x) + 1)));
        ROUNDTRIP(version, BSON("" << (static_cast<double>(x) + 1.1)));

        // Avoid negating signed integral minima
        if (i < 63)
            ROUNDTRIP(version, BSON("" << -static_cast<long long>(x)));

        if (i < 31)
            ROUNDTRIP(version, BSON("" << -static_cast<int>(x)));
        ROUNDTRIP(version, BSON("" << -static_cast<double>(x)));
        ROUNDTRIP(version, BSON("" << -(static_cast<double>(x) + .1)));

        ROUNDTRIP(version, BSON("" << -(static_cast<long long>(x) + 1)));
        ROUNDTRIP(version, BSON("" << -(static_cast<int>(x) + 1)));
        ROUNDTRIP(version, BSON("" << -(static_cast<double>(x) + 1)));
        ROUNDTRIP(version, BSON("" << -(static_cast<double>(x) + 1.1)));
    }
}

TEST_F(KeyStringTest, LotsOfNumbers2) {
    for (double i = -1100; i < 1100; i++) {
        double x = pow(2, i);
        ROUNDTRIP(version, BSON("" << x));
    }
    for (double i = -1100; i < 1100; i++) {
        double x = pow(2.1, i);
        ROUNDTRIP(version, BSON("" << x));
    }
}

TEST_F(KeyStringTest, RecordIdOrder1) {
    Ordering ordering = Ordering::make(BSON("a" << 1));

    KeyString a(version, BSON("" << 5), ordering, RecordId::min());
    KeyString b(version, BSON("" << 5), ordering, RecordId(2));
    KeyString c(version, BSON("" << 5), ordering, RecordId(3));
    KeyString d(version, BSON("" << 6), ordering, RecordId());
    KeyString e(version, BSON("" << 6), ordering, RecordId(1));

    ASSERT_LESS_THAN(a, b);
    ASSERT_LESS_THAN(b, c);
    ASSERT_LESS_THAN(c, d);
    ASSERT_LESS_THAN(d, e);
}

TEST_F(KeyStringTest, RecordIdOrder2) {
    Ordering ordering = Ordering::make(BSON("a" << -1 << "b" << -1));

    KeyString a(version, BSON("" << 5 << "" << 6), ordering, RecordId::min());
    KeyString b(version, BSON("" << 5 << "" << 6), ordering, RecordId(5));
    KeyString c(version, BSON("" << 5 << "" << 5), ordering, RecordId(4));
    KeyString d(version, BSON("" << 3 << "" << 4), ordering, RecordId(3));

    ASSERT_LESS_THAN(a, b);
    ASSERT_LESS_THAN(b, c);
    ASSERT_LESS_THAN(c, d);
    ASSERT_LESS_THAN(a, c);
    ASSERT_LESS_THAN(a, d);
    ASSERT_LESS_THAN(b, d);
}

TEST_F(KeyStringTest, RecordIdOrder2Double) {
    Ordering ordering = Ordering::make(BSON("a" << -1 << "b" << -1));

    KeyString a(version, BSON("" << 5.0 << "" << 6.0), ordering, RecordId::min());
    KeyString b(version, BSON("" << 5.0 << "" << 6.0), ordering, RecordId(5));
    KeyString c(version, BSON("" << 3.0 << "" << 4.0), ordering, RecordId(3));

    ASSERT_LESS_THAN(a, b);
    ASSERT_LESS_THAN(b, c);
    ASSERT_LESS_THAN(a, c);
}

TEST_F(KeyStringTest, Timestamp) {
    BSONObj a = BSON("" << Timestamp(0, 0));
    BSONObj b = BSON("" << Timestamp(1234, 1));
    BSONObj c = BSON("" << Timestamp(1234, 2));
    BSONObj d = BSON("" << Timestamp(1235, 1));
    BSONObj e = BSON("" << Timestamp(~0U, 0));

    {
        ROUNDTRIP(version, a);
        ROUNDTRIP(version, b);
        ROUNDTRIP(version, c);

        ASSERT_LESS_THAN(a, b);
        ASSERT_LESS_THAN(b, c);
        ASSERT_LESS_THAN(c, d);

        KeyString ka(version, a, ALL_ASCENDING);
        KeyString kb(version, b, ALL_ASCENDING);
        KeyString kc(version, c, ALL_ASCENDING);
        KeyString kd(version, d, ALL_ASCENDING);
        KeyString ke(version, e, ALL_ASCENDING);

        ASSERT(ka.compare(kb) < 0);
        ASSERT(kb.compare(kc) < 0);
        ASSERT(kc.compare(kd) < 0);
        ASSERT(kd.compare(ke) < 0);
    }

    {
        Ordering ALL_ASCENDING = Ordering::make(BSON("a" << -1));

        ROUNDTRIP(version, a);
        ROUNDTRIP(version, b);
        ROUNDTRIP(version, c);

        ASSERT(d.woCompare(c, ALL_ASCENDING) < 0);
        ASSERT(c.woCompare(b, ALL_ASCENDING) < 0);
        ASSERT(b.woCompare(a, ALL_ASCENDING) < 0);

        KeyString ka(version, a, ALL_ASCENDING);
        KeyString kb(version, b, ALL_ASCENDING);
        KeyString kc(version, c, ALL_ASCENDING);
        KeyString kd(version, d, ALL_ASCENDING);

        ASSERT(ka.compare(kb) > 0);
        ASSERT(kb.compare(kc) > 0);
        ASSERT(kc.compare(kd) > 0);
    }
}

TEST_F(KeyStringTest, AllTypesRoundtrip) {
    for (int i = 1; i <= JSTypeMax; i++) {
        {
            BSONObjBuilder b;
            b.appendMinForType("", i);
            BSONObj o = b.obj();
            ROUNDTRIP(version, o);
        }
        {
            BSONObjBuilder b;
            b.appendMaxForType("", i);
            BSONObj o = b.obj();
            ROUNDTRIP(version, o);
        }
    }
}

const std::vector<BSONObj>& getInterestingElements(KeyString::Version version) {
    static std::vector<BSONObj> elements;
    elements.clear();

    // These are used to test strings that include NUL bytes.
    const auto ball = "ball"_sd;
    const auto ball00n = "ball\0\0n"_sd;
    const auto zeroBall = "\0ball"_sd;

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
    elements.push_back(BSON(""
                            << "aaa"));
    elements.push_back(BSON(""
                            << "AAA"));
    elements.push_back(BSON("" << zeroBall));
    elements.push_back(BSON("" << ball));
    elements.push_back(BSON("" << ball00n));
    elements.push_back(BSON("" << BSONSymbol(zeroBall)));
    elements.push_back(BSON("" << BSONSymbol(ball)));
    elements.push_back(BSON("" << BSONSymbol(ball00n)));
    elements.push_back(BSON("" << BSON("a" << 5)));
    elements.push_back(BSON("" << BSON("a" << 6)));
    elements.push_back(BSON("" << BSON("b" << 6)));
    elements.push_back(BSON("" << BSON_ARRAY("a" << 5)));
    elements.push_back(BSON("" << BSONNULL));
    elements.push_back(BSON("" << BSONUndefined));
    elements.push_back(BSON("" << OID("abcdefabcdefabcdefabcdef")));
    elements.push_back(BSON("" << Date_t::fromMillisSinceEpoch(123)));
    elements.push_back(BSON("" << BSONCode("abc_code")));
    elements.push_back(BSON("" << BSONCode(zeroBall)));
    elements.push_back(BSON("" << BSONCode(ball)));
    elements.push_back(BSON("" << BSONCode(ball00n)));
    elements.push_back(BSON("" << BSONCodeWScope("def_code1",
                                                 BSON("x_scope"
                                                      << "a"))));
    elements.push_back(BSON("" << BSONCodeWScope("def_code2",
                                                 BSON("x_scope"
                                                      << "a"))));
    elements.push_back(BSON("" << BSONCodeWScope("def_code2",
                                                 BSON("x_scope"
                                                      << "b"))));
    elements.push_back(BSON("" << BSONCodeWScope(zeroBall, BSON("a" << 1))));
    elements.push_back(BSON("" << BSONCodeWScope(ball, BSON("a" << 1))));
    elements.push_back(BSON("" << BSONCodeWScope(ball00n, BSON("a" << 1))));
    elements.push_back(BSON("" << true));
    elements.push_back(BSON("" << false));

    // Something that needs multiple bytes of typeBits
    elements.push_back(BSON("" << BSON_ARRAY("" << BSONSymbol("") << 0 << 0ll << 0.0 << -0.0)));

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

        if (powerOfTwo <= 52) {  // is dNum - 0.5 representable?
            elements.push_back(BSON("" << (dNum - 0.5)));
            elements.push_back(BSON("" << -(dNum - 0.5)));
            elements.push_back(BSON("" << (dNum - 0.1)));
            elements.push_back(BSON("" << -(dNum - 0.1)));
        }

        if (powerOfTwo <= 51) {  // is dNum + 0.5 representable?
            elements.push_back(BSON("" << (dNum + 0.5)));
            elements.push_back(BSON("" << -(dNum + 0.5)));
            elements.push_back(BSON("" << (dNum + 0.1)));
            elements.push_back(BSON("" << -(dNum + -.1)));
        }

        if (version != KeyString::Version::V0) {
            const Decimal128 dec(static_cast<int64_t>(lNum));
            const Decimal128 one("1");
            const Decimal128 half("0.5");
            const Decimal128 tenth("0.1");
            elements.push_back(BSON("" << dec));
            elements.push_back(BSON("" << dec.add(one)));
            elements.push_back(BSON("" << dec.subtract(one)));
            elements.push_back(BSON("" << dec.negate()));
            elements.push_back(BSON("" << dec.add(one).negate()));
            elements.push_back(BSON("" << dec.subtract(one).negate()));
            elements.push_back(BSON("" << dec.subtract(half)));
            elements.push_back(BSON("" << dec.subtract(half).negate()));
            elements.push_back(BSON("" << dec.add(half)));
            elements.push_back(BSON("" << dec.add(half).negate()));
            elements.push_back(BSON("" << dec.subtract(tenth)));
            elements.push_back(BSON("" << dec.subtract(tenth).negate()));
            elements.push_back(BSON("" << dec.add(tenth)));
            elements.push_back(BSON("" << dec.add(tenth).negate()));
        }
    }

    {
        // Numbers around +/- numeric_limits<long long>::max() which can't be represented
        // precisely as a double.
        const long long maxLL = std::numeric_limits<long long>::max();
        const double closestAbove = 9223372036854775808.0;  // 2**63
        const double closestBelow = 9223372036854774784.0;  // 2**63 - epsilon

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
        const double closestBelow = -9223372036854777856.0;  // -2**63 - epsilon
        const double equal = -9223372036854775808.0;         // 2**63
        const double closestAbove = -9223372036854774784.0;  // -2**63 + epsilon

        elements.push_back(BSON("" << minLL));
        elements.push_back(BSON("" << equal));
        elements.push_back(BSON("" << closestAbove));
        elements.push_back(BSON("" << closestBelow));
    }

    if (version != KeyString::Version::V0) {
        // Numbers that are hard to round to between binary and decimal.
        elements.push_back(BSON("" << 0.1));
        elements.push_back(BSON("" << Decimal128("0.100000000")));
        // Decimals closest to the double representation of 0.1.
        elements.push_back(BSON("" << Decimal128("0.1000000000000000055511151231257827")));
        elements.push_back(BSON("" << Decimal128("0.1000000000000000055511151231257828")));

        // Numbers close to numerical underflow/overflow for double.
        elements.push_back(BSON("" << Decimal128("1.797693134862315708145274237317044E308")));
        elements.push_back(BSON("" << Decimal128("1.797693134862315708145274237317043E308")));
        elements.push_back(BSON("" << Decimal128("-1.797693134862315708145274237317044E308")));
        elements.push_back(BSON("" << Decimal128("-1.797693134862315708145274237317043E308")));
        elements.push_back(BSON("" << Decimal128("9.881312916824930883531375857364427")));
        elements.push_back(BSON("" << Decimal128("9.881312916824930883531375857364428")));
        elements.push_back(BSON("" << Decimal128("-9.881312916824930883531375857364427")));
        elements.push_back(BSON("" << Decimal128("-9.881312916824930883531375857364428")));
        elements.push_back(BSON("" << Decimal128("4.940656458412465441765687928682213E-324")));
        elements.push_back(BSON("" << Decimal128("4.940656458412465441765687928682214E-324")));
        elements.push_back(BSON("" << Decimal128("-4.940656458412465441765687928682214E-324")));
        elements.push_back(BSON("" << Decimal128("-4.940656458412465441765687928682213E-324")));
    }

    // Tricky double precision number for binary/decimal conversion: very close to a decimal
    if (version != KeyString::Version::V0)
        elements.push_back(BSON("" << Decimal128("3743626360493413E-165")));
    elements.push_back(BSON("" << 3743626360493413E-165));

    return elements;
}

void testPermutation(KeyString::Version version,
                     const std::vector<BSONObj>& elementsOrig,
                     const std::vector<BSONObj>& orderings,
                     bool debug) {
    // Since KeyStrings are compared using memcmp we can assume it provides a total ordering such
    // that there won't be cases where (a < b && b < c && !(a < c)). This test still needs to ensure
    // that it provides the *correct* total ordering.
    for (size_t k = 0; k < orderings.size(); k++) {
        BSONObj orderObj = orderings[k];
        Ordering ordering = Ordering::make(orderObj);
        if (debug)
            log() << "ordering: " << orderObj;

        std::vector<BSONObj> elements = elementsOrig;
        std::stable_sort(elements.begin(), elements.end(), BSONObjCmp(orderObj));

        for (size_t i = 0; i < elements.size(); i++) {
            const BSONObj& o1 = elements[i];
            if (debug)
                log() << "\to1: " << o1;
            ROUNDTRIP_ORDER(version, o1, ordering);

            KeyString k1(version, o1, ordering);

            KeyString l1(version, BSON("l" << o1.firstElement()), ordering);  // kLess
            KeyString g1(version, BSON("g" << o1.firstElement()), ordering);  // kGreater
            ASSERT_LT(l1, k1);
            ASSERT_GT(g1, k1);

            if (i + 1 < elements.size()) {
                const BSONObj& o2 = elements[i + 1];
                if (debug)
                    log() << "\t\t o2: " << o2;
                KeyString k2(version, o2, ordering);
                KeyString g2(version, BSON("g" << o2.firstElement()), ordering);
                KeyString l2(version, BSON("l" << o2.firstElement()), ordering);

                int bsonCmp = o1.woCompare(o2, ordering);
                invariant(bsonCmp <= 0);  // We should be sorted...

                if (bsonCmp == 0) {
                    ASSERT_EQ(k1, k2);
                } else {
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
                } else {
                    // k1 is less than k2. Less(k2) and Greater(k1) should be between them.
                    ASSERT_LT(g1, k2);
                    ASSERT_GT(l2, k1);
                }
            }
        }
    }
}

TEST_F(KeyStringTest, AllPermCompare) {
    const std::vector<BSONObj>& elements = getInterestingElements(version);

    for (size_t i = 0; i < elements.size(); i++) {
        const BSONObj& o = elements[i];
        ROUNDTRIP(version, o);
    }

    std::vector<BSONObj> orderings;
    orderings.push_back(BSON("a" << 1));
    orderings.push_back(BSON("a" << -1));

    testPermutation(version, elements, orderings, false);
}

TEST_F(KeyStringTest, AllPerm2Compare) {
#if !defined(MONGO_CONFIG_OPTIMIZED_BUILD)
    log() << "\t\t\tskipping permutation testing on non-optimized build";
    return;
#endif

    const std::vector<BSONObj>& baseElements = getInterestingElements(version);

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

    log() << "AllPerm2Compare " << KeyString::versionToString(version)
          << " size:" << elements.size();

    for (size_t i = 0; i < elements.size(); i++) {
        const BSONObj& o = elements[i];
        ROUNDTRIP(version, o);
    }

    std::vector<BSONObj> orderings;
    orderings.push_back(BSON("a" << 1 << "b" << 1));
    orderings.push_back(BSON("a" << -1 << "b" << 1));
    orderings.push_back(BSON("a" << 1 << "b" << -1));
    orderings.push_back(BSON("a" << -1 << "b" << -1));

    testPermutation(version, elements, orderings, false);
}

#define COMPARE_HELPER(LHS, RHS) (((LHS) < (RHS)) ? -1 : (((LHS) == (RHS)) ? 0 : 1))

int compareLongToDouble(long long lhs, double rhs) {
    if (rhs >= std::numeric_limits<long long>::max())
        return -1;
    if (rhs < std::numeric_limits<long long>::min())
        return 1;

    if (fabs(rhs) >= (1LL << 52)) {
        return COMPARE_HELPER(lhs, static_cast<long long>(rhs));
    }

    return COMPARE_HELPER(static_cast<double>(lhs), rhs);
}

int compareNumbers(const BSONElement& lhs, const BSONElement& rhs) {
    invariant(lhs.isNumber());
    invariant(rhs.isNumber());

    if (lhs.type() == NumberInt || lhs.type() == NumberLong) {
        if (rhs.type() == NumberInt || rhs.type() == NumberLong) {
            return COMPARE_HELPER(lhs.numberLong(), rhs.numberLong());
        }
        return compareLongToDouble(lhs.numberLong(), rhs.Double());
    } else {  // double
        if (rhs.type() == NumberDouble) {
            return COMPARE_HELPER(lhs.Double(), rhs.Double());
        }
        return -compareLongToDouble(rhs.numberLong(), lhs.Double());
    }
}

TEST_F(KeyStringTest, NaNs) {
    // TODO use hex floats to force distinct NaNs
    const double nan1 = std::numeric_limits<double>::quiet_NaN();
    const double nan2 = std::numeric_limits<double>::signaling_NaN();

    // Since only output a single NaN, we can't use the normal ROUNDTRIP testing here.

    const KeyString ks1a(version, BSON("" << nan1), ONE_ASCENDING);
    const KeyString ks1d(version, BSON("" << nan1), ONE_DESCENDING);

    const KeyString ks2a(version, BSON("" << nan2), ONE_ASCENDING);
    const KeyString ks2d(version, BSON("" << nan2), ONE_DESCENDING);

    ASSERT_EQ(ks1a, ks2a);
    ASSERT_EQ(ks1d, ks2d);

    ASSERT(std::isnan(toBson(ks1a, ONE_ASCENDING)[""].Double()));
    ASSERT(std::isnan(toBson(ks2a, ONE_ASCENDING)[""].Double()));
    ASSERT(std::isnan(toBson(ks1d, ONE_DESCENDING)[""].Double()));
    ASSERT(std::isnan(toBson(ks2d, ONE_DESCENDING)[""].Double()));
}
TEST_F(KeyStringTest, NumberOrderLots) {
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

            // Avoid negating signed integral minima
            if (i < 63)
                numbers.push_back(BSON("" << -static_cast<long long>(x)));

            if (i < 31)
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
        keyStrings.push_back(new KeyString(version, numbers[i], ordering));
    }

    for (size_t i = 0; i < numbers.size(); i++) {
        for (size_t j = 0; j < numbers.size(); j++) {
            const KeyString& a = *keyStrings[i];
            const KeyString& b = *keyStrings[j];
            ASSERT_EQUALS(a.compare(b), -b.compare(a));

            if (a.compare(b) !=
                compareNumbers(numbers[i].firstElement(), numbers[j].firstElement())) {
                log() << numbers[i] << " " << numbers[j];
            }

            ASSERT_EQUALS(a.compare(b),
                          compareNumbers(numbers[i].firstElement(), numbers[j].firstElement()));
        }
    }
}

TEST_F(KeyStringTest, RecordIds) {
    for (int i = 0; i < 63; i++) {
        const RecordId rid = RecordId(1ll << i);

        {  // Test encoding / decoding of single RecordIds
            const KeyString ks(version, rid);
            ASSERT_GTE(ks.getSize(), 2u);
            ASSERT_LTE(ks.getSize(), 10u);

            ASSERT_EQ(KeyString::decodeRecordIdAtEnd(ks.getBuffer(), ks.getSize()), rid);

            {
                BufReader reader(ks.getBuffer(), ks.getSize());
                ASSERT_EQ(KeyString::decodeRecordId(&reader), rid);
                ASSERT(reader.atEof());
            }

            if (rid.isNormal()) {
                ASSERT_GT(ks, KeyString(version, RecordId()));
                ASSERT_GT(ks, KeyString(version, RecordId::min()));
                ASSERT_LT(ks, KeyString(version, RecordId::max()));

                ASSERT_GT(ks, KeyString(version, RecordId(rid.repr() - 1)));
                ASSERT_LT(ks, KeyString(version, RecordId(rid.repr() + 1)));
            }
        }

        for (int j = 0; j < 63; j++) {
            RecordId other = RecordId(1ll << j);

            if (rid == other)
                ASSERT_EQ(KeyString(version, rid), KeyString(version, other));
            if (rid < other)
                ASSERT_LT(KeyString(version, rid), KeyString(version, other));
            if (rid > other)
                ASSERT_GT(KeyString(version, rid), KeyString(version, other));

            {
                // Test concatenating RecordIds like in a unique index.
                KeyString ks(version);
                ks.appendRecordId(RecordId::max());  // uses all bytes
                ks.appendRecordId(rid);
                ks.appendRecordId(RecordId(0xDEADBEEF));  // uses some extra bytes
                ks.appendRecordId(rid);
                ks.appendRecordId(RecordId(1));  // uses no extra bytes
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

namespace {
const uint64_t kMinPerfMicros = 10 * 1000;
const uint64_t kMinPerfSamples = 10 * 1000;
typedef std::vector<BSONObj> Numbers;

/**
 * Evaluates ROUNDTRIP on all items in Numbers a sufficient number of times to take at least
 * kMinPerfMicros microseconds. Logs the elapsed time per ROUNDTRIP evaluation.
 */
void perfTest(KeyString::Version version, const Numbers& numbers) {
    uint64_t micros = 0;
    uint64_t iters;
    // Ensure at least 16 iterations are done and at least 25 milliseconds is timed
    for (iters = 16; iters < (1 << 30) && micros < kMinPerfMicros; iters *= 2) {
        // Measure the number of loops
        Timer t;

        for (uint64_t i = 0; i < iters; i++)
            for (auto item : numbers) {
                // Assuming there are sufficient invariants in the to/from KeyString methods
                // that calls will not be optimized away.
                const KeyString ks(version, item, ALL_ASCENDING);
                const BSONObj& converted = toBson(ks, ALL_ASCENDING);
                invariant(converted.binaryEqual(item));
            }

        micros = t.micros();
    }

    auto minmax = std::minmax_element(numbers.begin(), numbers.end());

    log() << 1E3 * micros / static_cast<double>(iters * numbers.size()) << " ns per "
          << mongo::KeyString::versionToString(version) << " roundtrip"
          << (kDebugBuild ? " (DEBUG BUILD!)" : "") << " min " << (*minmax.first)[""] << ", max"
          << (*minmax.second)[""];
}
}  // namespace

TEST_F(KeyStringTest, CommonIntPerf) {
    // Exponential distribution, so skewed towards smaller integers.
    std::random_device rd;
    std::mt19937 gen(rd());
    std::exponential_distribution<double> expReal(1e-3);

    std::vector<BSONObj> numbers;
    for (uint64_t x = 0; x < kMinPerfSamples; x++)
        numbers.push_back(BSON("" << static_cast<int>(expReal(gen))));

    perfTest(version, numbers);
}

TEST_F(KeyStringTest, UniformInt64Perf) {
    std::vector<BSONObj> numbers;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<long long> uniformInt64(std::numeric_limits<long long>::min(),
                                                          std::numeric_limits<long long>::max());

    for (uint64_t x = 0; x < kMinPerfSamples; x++)
        numbers.push_back(BSON("" << uniformInt64(gen)));

    perfTest(version, numbers);
}

TEST_F(KeyStringTest, CommonDoublePerf) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::exponential_distribution<double> expReal(1e-3);

    std::vector<BSONObj> numbers;
    for (uint64_t x = 0; x < kMinPerfSamples; x++)
        numbers.push_back(BSON("" << expReal(gen)));

    perfTest(version, numbers);
}

TEST_F(KeyStringTest, UniformDoublePerf) {
    std::vector<BSONObj> numbers;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<long long> uniformInt64(std::numeric_limits<long long>::min(),
                                                          std::numeric_limits<long long>::max());

    for (uint64_t x = 0; x < kMinPerfSamples; x++) {
        uint64_t u = uniformInt64(gen);
        double d;
        memcpy(&d, &u, sizeof(d));
        if (std::isnormal(d))
            numbers.push_back(BSON("" << d));
    }
    perfTest(version, numbers);
}

TEST_F(KeyStringTest, CommonDecimalPerf) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::exponential_distribution<double> expReal(1e-3);

    if (version == KeyString::Version::V0)
        return;

    std::vector<BSONObj> numbers;
    for (uint64_t x = 0; x < kMinPerfSamples; x++)
        numbers.push_back(
            BSON("" << Decimal128(
                           expReal(gen), Decimal128::kRoundTo34Digits, Decimal128::kRoundTiesToAway)
                           .quantize(Decimal128("0.01", Decimal128::kRoundTiesToAway))));

    perfTest(version, numbers);
}

TEST_F(KeyStringTest, UniformDecimalPerf) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<long long> uniformInt64(std::numeric_limits<long long>::min(),
                                                          std::numeric_limits<long long>::max());

    if (version == KeyString::Version::V0)
        return;

    std::vector<BSONObj> numbers;
    for (uint64_t x = 0; x < kMinPerfSamples; x++) {
        uint64_t hi = uniformInt64(gen);
        uint64_t lo = uniformInt64(gen);
        Decimal128 d(Decimal128::Value{lo, hi});
        if (!d.isZero() && !d.isNaN() && !d.isInfinite())
            numbers.push_back(BSON("" << d));
    }
    perfTest(version, numbers);
}
