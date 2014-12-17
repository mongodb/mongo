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

#include "mongo/s/type_lockpings.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace {

    using std::string;
    using mongo::BSONObj;
    using mongo::LockpingsType;
    using mongo::Date_t;

    TEST(Validity, MissingFields) {
        LockpingsType lockPing;
        BSONObj objNoProcess = BSON(LockpingsType::ping(time(0)));
        string errMsg;
        ASSERT(lockPing.parseBSON(objNoProcess, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(lockPing.isValid(NULL));

        BSONObj objNoPing = BSON(LockpingsType::process("host.local:27017:1352918870:16807"));
        ASSERT(lockPing.parseBSON(objNoPing, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(lockPing.isValid(NULL));
    }

    TEST(Validity, Valid) {
        LockpingsType lockPing;
        BSONObj obj = BSON(LockpingsType::process("host.local:27017:1352918870:16807") <<
                           LockpingsType::ping(1ULL));
        string errMsg;
        ASSERT(lockPing.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(lockPing.isValid(NULL));
        ASSERT_EQUALS(lockPing.getProcess(), "host.local:27017:1352918870:16807");
        ASSERT_EQUALS(lockPing.getPing(), 1ULL);
    }

    TEST(Validity, BadType) {
        LockpingsType lockPing;
        BSONObj obj = BSON(LockpingsType::process() << 0);
        string errMsg;
        ASSERT((!lockPing.parseBSON(obj, &errMsg)) && (errMsg != ""));
    }

} // unnamed namespace
