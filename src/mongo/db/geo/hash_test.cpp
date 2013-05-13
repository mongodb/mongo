/**
 *    Copyright (C) 2012 10gen Inc.
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
 */

/**
 * This file contains tests for mongo/db/geo/hash.cpp.
 */

#include <string>
#include <sstream>

#include "mongo/db/geo/hash.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

using mongo::GeoHash;
using std::string;
using std::stringstream;

namespace {
    TEST(GeoHash, MakeZeroHash) {
        unsigned x = 0, y = 0;
        GeoHash hash(x, y);
    }

    static string makeRandomBitString(int length) {
        stringstream ss;
        mongo::PseudoRandom random(31337);
        for (int i = 0; i < length; ++i) {
            if (random.nextInt32() & 1) {
                ss << "1";
            } else {
                ss << "0";
            }
        }
        return ss.str();
    }

    TEST(GeoHash, MakeRandomValidHashes) {
        int maxStringLength = 64;
        for (int i = 0; i < maxStringLength; i += 2) {
            string a = makeRandomBitString(i);
            GeoHash hashA = GeoHash(a);
            (void)hashA.isBitSet(i, 0);
            (void)hashA.isBitSet(i, 1);
        }
    }

    // ASSERT_THROWS does not work if we try to put GeoHash(a) in the macro.
    static GeoHash makeHash(const string& a) { return GeoHash(a); }

    TEST(GeoHash, MakeTooLongHash) {
        string a = makeRandomBitString(100);
        ASSERT_THROWS(makeHash(a), mongo::UserException);
    }

    TEST(GeoHash, MakeOddHash) {
        string a = makeRandomBitString(13);
        ASSERT_THROWS(makeHash(a), mongo::UserException);
    }
}
