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

#include "mongo/bson/util/misc.h" // for Date_t
#include "mongo/s/type_mongos.h"
#include "mongo/unittest/unittest.h"

namespace {

    using std::string;
    using mongo::BSONObj;
    using mongo::MongosType;
    using mongo::Date_t;

    TEST(Validity, MissingFields) {
        MongosType mongos;
        BSONObj objNoName = BSON(MongosType::ping(time(0)) <<
                                 MongosType::up(100) <<
                                 MongosType::waiting(false));
        string errMsg;
        ASSERT(mongos.parseBSON(objNoName, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(mongos.isValid(NULL));

        BSONObj objNoPing = BSON(MongosType::name("localhost:27017") <<
                                 MongosType::up(100) <<
                                 MongosType::waiting(false));
        ASSERT(mongos.parseBSON(objNoPing, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(mongos.isValid(NULL));

        BSONObj objNoUp = BSON(MongosType::name("localhost:27017") <<
                               MongosType::ping(time(0)) <<
                               MongosType::waiting(false));
        ASSERT(mongos.parseBSON(objNoUp, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(mongos.isValid(NULL));
    }

    TEST(Validity, Valid) {
        MongosType mongos;
        BSONObj obj = BSON(MongosType::name("localhost:27017") <<
                           MongosType::ping(1ULL) <<
                           MongosType::up(100) <<
                           MongosType::waiting(false));
        string errMsg;
        ASSERT(mongos.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(mongos.isValid(NULL));
        ASSERT_EQUALS(mongos.getName(), "localhost:27017");
        ASSERT_EQUALS(mongos.getPing(), 1ULL);
        ASSERT_EQUALS(mongos.getUp(), 100);
        ASSERT_EQUALS(mongos.getWaiting(), false);
    }

    TEST(Validity, BadType) {
        MongosType mongos;
        BSONObj obj = BSON(MongosType::name() << 0);
        string errMsg;
        ASSERT((!mongos.parseBSON(obj, &errMsg)) && (errMsg != ""));
    }

} // unnamed namespace
