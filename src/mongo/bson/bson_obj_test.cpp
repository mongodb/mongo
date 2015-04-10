/*    Copyright 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"

#include "mongo/unittest/unittest.h"

namespace {
    using namespace mongo;

    TEST(BSONObjToString, EmptyArray) {
        const char text[] = "{ x: [] }";
        mongo::BSONObj o1 = mongo::fromjson(text);
        const std::string o1_str = o1.toString();
        ASSERT_EQUALS(text, o1_str);
    }

    TEST(BSONObjCompare, NumberDouble) {
        ASSERT_LT(BSON("" << 0.0), BSON("" << 1.0));
        ASSERT_LT(BSON("" << -1.0), BSON("" << 0.0));
        ASSERT_LT(BSON("" << -1.0), BSON("" << 1.0));

        ASSERT_LT(BSON("" << 0.0), BSON("" << 0.1));
        ASSERT_LT(BSON("" << 0.1), BSON("" << 1.0));
        ASSERT_LT(BSON("" << -1.0), BSON("" << -0.1));
        ASSERT_LT(BSON("" << -0.1), BSON("" << 0.0));
        ASSERT_LT(BSON("" << -0.1), BSON("" << 0.1));

        ASSERT_LT(BSON("" << 0.0), BSON("" << std::numeric_limits<double>::denorm_min()));
        ASSERT_GT(BSON("" << 0.0), BSON("" << -std::numeric_limits<double>::denorm_min()));

        ASSERT_LT(BSON("" << 1.0), BSON("" << (1.0 + std::numeric_limits<double>::epsilon())));
        ASSERT_GT(BSON("" << -1.0), BSON("" << (-1.0 - std::numeric_limits<double>::epsilon())));

        ASSERT_EQ(BSON("" << 0.0), BSON("" << -0.0));

        ASSERT_GT(BSON("" << std::numeric_limits<double>::infinity()), BSON("" << 0.0));
        ASSERT_GT(BSON("" << std::numeric_limits<double>::infinity()),
                  BSON("" << std::numeric_limits<double>::max())); // max is finite
        ASSERT_GT(BSON("" << std::numeric_limits<double>::infinity()),
                  BSON("" << -std::numeric_limits<double>::infinity()));

        ASSERT_LT(BSON("" << -std::numeric_limits<double>::infinity()), BSON("" << 0.0));
        ASSERT_LT(BSON("" << -std::numeric_limits<double>::infinity()),
                  BSON("" << -std::numeric_limits<double>::max()));
        ASSERT_LT(BSON("" << -std::numeric_limits<double>::infinity()),
                  BSON("" << std::numeric_limits<double>::infinity()));

        ASSERT_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()), BSON("" << 0.0));
        ASSERT_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()),
                  BSON("" << -std::numeric_limits<double>::max()));
        ASSERT_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()),
                  BSON("" << std::numeric_limits<double>::infinity()));
        ASSERT_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()),
                  BSON("" << -std::numeric_limits<double>::infinity()));

        // TODO in C++11 use hex floating point to test distinct NaN representations
        ASSERT_EQ(BSON("" << std::numeric_limits<double>::quiet_NaN()),
                  BSON("" << std::numeric_limits<double>::signaling_NaN()));
    }

    TEST(BSONObjCompare, NumberLong_Double) {
        ASSERT_EQ(BSON("" << 0ll), BSON("" << 0.0));
        ASSERT_EQ(BSON("" << 0ll), BSON("" << -0.0));

        ASSERT_EQ(BSON("" << 1ll), BSON("" << 1.0));
        ASSERT_EQ(BSON("" << -1ll), BSON("" << -1.0));

        ASSERT_LT(BSON("" << 0ll), BSON("" << 1.0));
        ASSERT_LT(BSON("" << -1ll), BSON("" << 0.0));
        ASSERT_LT(BSON("" << -1ll), BSON("" << 1.0));

        ASSERT_LT(BSON("" << 0ll), BSON("" << 0.1));
        ASSERT_LT(BSON("" << 0.1), BSON("" << 1ll));
        ASSERT_LT(BSON("" << -1ll), BSON("" << -0.1));
        ASSERT_LT(BSON("" << -0.1), BSON("" << 0ll));

        ASSERT_LT(BSON("" << 0ll), BSON("" << std::numeric_limits<double>::denorm_min()));
        ASSERT_GT(BSON("" << 0ll), BSON("" << -std::numeric_limits<double>::denorm_min()));

        ASSERT_LT(BSON("" << 1ll), BSON("" << (1.0 + std::numeric_limits<double>::epsilon())));
        ASSERT_GT(BSON("" << -1ll), BSON("" << (-1.0 - std::numeric_limits<double>::epsilon())));

        ASSERT_GT(BSON("" << std::numeric_limits<double>::infinity()), BSON("" << 0ll));
        ASSERT_GT(BSON("" << std::numeric_limits<double>::infinity()),
                  BSON("" << std::numeric_limits<long long>::max()));
        ASSERT_GT(BSON("" << std::numeric_limits<double>::infinity()),
                  BSON("" << std::numeric_limits<long long>::min()));

        ASSERT_LT(BSON("" << -std::numeric_limits<double>::infinity()), BSON("" << 0ll));
        ASSERT_LT(BSON("" << -std::numeric_limits<double>::infinity()),
                  BSON("" << std::numeric_limits<long long>::max()));
        ASSERT_LT(BSON("" << -std::numeric_limits<double>::infinity()),
                  BSON("" << std::numeric_limits<long long>::min()));

        ASSERT_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()), BSON("" << 0ll));
        ASSERT_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()),
                  BSON("" << std::numeric_limits<long long>::min()));

        for (int powerOfTwo = 0; powerOfTwo < 63; powerOfTwo++) {
            const long long lNum = 1ll << powerOfTwo;
            const double dNum = double(lNum);

            // All powers of two in this range can be represented exactly as doubles.
            invariant(lNum == static_cast<long long>(dNum));

            ASSERT_EQ(BSON("" << lNum), BSON("" << dNum));
            ASSERT_EQ(BSON("" << -lNum), BSON("" << -dNum));

            ASSERT_GT(BSON("" << (lNum + 1)), BSON("" << dNum));
            ASSERT_LT(BSON("" << (lNum - 1)), BSON("" << dNum));
            ASSERT_GT(BSON("" << (-lNum + 1)), BSON("" << -dNum));
            ASSERT_LT(BSON("" << (-lNum - 1)), BSON("" << -dNum));

            if (powerOfTwo <= 52) { // is dNum - 0.5 representable?
                ASSERT_GT(BSON("" << lNum), BSON("" << (dNum - 0.5)));
                ASSERT_LT(BSON("" << -lNum), BSON("" << -(dNum - 0.5)));
            }

            if (powerOfTwo <= 51) { // is dNum + 0.5 representable?
                ASSERT_LT(BSON("" << lNum), BSON("" << (dNum + 0.5)));
                ASSERT_GT(BSON("" << -lNum), BSON("" << -(dNum + 0.5)));
            }
        }

        {
            // Numbers around +/- numeric_limits<long long>::max() which can't be represented
            // precisely as a double.
            const long long maxLL = std::numeric_limits<long long>::max();
            const double closestAbove = 9223372036854775808.0; // 2**63
            const double closestBelow = 9223372036854774784.0; // 2**63 - epsilon

            ASSERT_GT(BSON("" << maxLL), BSON("" << (maxLL - 1)));
            ASSERT_LT(BSON("" << maxLL), BSON("" << closestAbove));
            ASSERT_GT(BSON("" << maxLL), BSON("" << closestBelow));

            ASSERT_LT(BSON("" << -maxLL), BSON("" << -(maxLL - 1)));
            ASSERT_GT(BSON("" << -maxLL), BSON("" << -closestAbove));
            ASSERT_LT(BSON("" << -maxLL), BSON("" << -closestBelow));
        }

        {
            // Numbers around numeric_limits<long long>::min() which can be represented precisely as
            // a double, but not as a positive long long.
            const long long minLL = std::numeric_limits<long long>::min();
            const double closestBelow = -9223372036854777856.0; // -2**63 - epsilon
            const double equal = -9223372036854775808.0; // 2**63
            const double closestAbove = -9223372036854774784.0; // -2**63 + epsilon

            invariant(static_cast<double>(minLL) == equal);
            invariant(static_cast<long long>(equal) == minLL);

            ASSERT_LT(BSON("" << minLL), BSON("" << (minLL + 1)));

            ASSERT_EQ(BSON("" << minLL), BSON("" << equal));
            ASSERT_LT(BSON("" << minLL), BSON("" << closestAbove));
            ASSERT_GT(BSON("" << minLL), BSON("" << closestBelow));
        }
    }

    TEST(Looping, Cpp11Basic) {
        int count = 0;
        for (BSONElement e : BSON("a" << 1 << "a" << 2 << "a" << 3)) {
            ASSERT_EQUALS( e.fieldNameStringData() , "a" );
            count += e.Int();
        }

        ASSERT_EQUALS( count , 1 + 2 + 3 );
    }

    TEST(Looping, Cpp11Auto) {
        int count = 0;
        for (auto e : BSON("a" << 1 << "a" << 2 << "a" << 3)) {
            ASSERT_EQUALS( e.fieldNameStringData() , "a" );
            count += e.Int();
        }

        ASSERT_EQUALS( count , 1 + 2 + 3 );
    }

} // unnamed namespace
