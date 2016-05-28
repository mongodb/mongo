/*    Copyright 2014 MongoDB Inc.
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

#include "mongo/db/keypattern.h"

#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(KeyPattern, ExtendRangeBound) {
    BSONObj bound = BSON("a" << 55);
    BSONObj longBound = BSON("a" << 55 << "b" << 66);

    // test keyPattern shorter than bound, should fail
    {
        KeyPattern keyPat(BSON("a" << 1));
        ASSERT_THROWS(keyPat.extendRangeBound(longBound, false), MsgAssertionException);
    }

    // test keyPattern doesn't match bound, should fail
    {
        KeyPattern keyPat(BSON("b" << 1));
        ASSERT_THROWS(keyPat.extendRangeBound(bound, false), MsgAssertionException);
    }
    {
        KeyPattern keyPat(BSON("a" << 1 << "c" << 1));
        ASSERT_THROWS(keyPat.extendRangeBound(longBound, false), MsgAssertionException);
    }

    // test keyPattern same as bound
    {
        KeyPattern keyPat(BSON("a" << 1));
        BSONObj newB = keyPat.extendRangeBound(bound, false);
        ASSERT_EQUALS(newB, BSON("a" << 55));
    }
    {
        KeyPattern keyPat(BSON("a" << 1));
        BSONObj newB = keyPat.extendRangeBound(bound, false);
        ASSERT_EQUALS(newB, BSON("a" << 55));
    }

    // test keyPattern longer than bound, simple
    {
        KeyPattern keyPat(BSON("a" << 1 << "b" << 1));
        BSONObj newB = keyPat.extendRangeBound(bound, false);
        ASSERT_EQUALS(newB, BSON("a" << 55 << "b" << MINKEY));
    }
    {
        KeyPattern keyPat(BSON("a" << 1 << "b" << 1));
        BSONObj newB = keyPat.extendRangeBound(bound, true);
        ASSERT_EQUALS(newB, BSON("a" << 55 << "b" << MAXKEY));
    }

    // test keyPattern longer than bound, more complex pattern directions
    {
        KeyPattern keyPat(BSON("a" << 1 << "b" << -1));
        BSONObj newB = keyPat.extendRangeBound(bound, false);
        ASSERT_EQUALS(newB, BSON("a" << 55 << "b" << MAXKEY));
    }
    {
        KeyPattern keyPat(BSON("a" << 1 << "b" << -1));
        BSONObj newB = keyPat.extendRangeBound(bound, true);
        ASSERT_EQUALS(newB, BSON("a" << 55 << "b" << MINKEY));
    }
    {
        KeyPattern keyPat(BSON("a" << 1 << "b" << -1 << "c" << 1));
        BSONObj newB = keyPat.extendRangeBound(bound, false);
        ASSERT_EQUALS(newB, BSON("a" << 55 << "b" << MAXKEY << "c" << MINKEY));
    }
    {
        KeyPattern keyPat(BSON("a" << 1 << "b" << -1 << "c" << 1));
        BSONObj newB = keyPat.extendRangeBound(bound, true);
        ASSERT_EQUALS(newB, BSON("a" << 55 << "b" << MINKEY << "c" << MAXKEY));
    }
}

TEST(KeyPattern, GlobalMinMax) {
    //
    // Simple KeyPatterns
    //

    ASSERT_EQUALS(KeyPattern(BSON("a" << 1)).globalMin(), BSON("a" << MINKEY));
    ASSERT_EQUALS(KeyPattern(BSON("a" << 1)).globalMax(), BSON("a" << MAXKEY));

    ASSERT_EQUALS(KeyPattern(BSON("a" << -1)).globalMin(), BSON("a" << MAXKEY));
    ASSERT_EQUALS(KeyPattern(BSON("a" << -1)).globalMax(), BSON("a" << MINKEY));

    ASSERT_EQUALS(KeyPattern(BSON("a" << 1 << "b" << 1.0)).globalMin(),
                  BSON("a" << MINKEY << "b" << MINKEY));
    ASSERT_EQUALS(KeyPattern(BSON("a" << 1 << "b" << 1.0)).globalMax(),
                  BSON("a" << MAXKEY << "b" << MAXKEY));

    ASSERT_EQUALS(KeyPattern(BSON("a" << 1 << "b" << -1.0f)).globalMin(),
                  BSON("a" << MINKEY << "b" << MAXKEY));
    ASSERT_EQUALS(KeyPattern(BSON("a" << 1 << "b" << -1.0f)).globalMax(),
                  BSON("a" << MAXKEY << "b" << MINKEY));

    ASSERT_EQUALS(KeyPattern(BSON("a"
                                  << "hashed"))
                      .globalMin(),
                  BSON("a" << MINKEY));
    ASSERT_EQUALS(KeyPattern(BSON("a"
                                  << "hashed"))
                      .globalMax(),
                  BSON("a" << MAXKEY));

    //
    // Nested KeyPatterns
    //

    ASSERT_EQUALS(KeyPattern(BSON("a.b" << 1)).globalMin(), BSON("a.b" << MINKEY));
    ASSERT_EQUALS(KeyPattern(BSON("a.b" << 1)).globalMax(), BSON("a.b" << MAXKEY));

    ASSERT_EQUALS(KeyPattern(BSON("a.b.c" << -1)).globalMin(), BSON("a.b.c" << MAXKEY));
    ASSERT_EQUALS(KeyPattern(BSON("a.b.c" << -1)).globalMax(), BSON("a.b.c" << MINKEY));
}
}
