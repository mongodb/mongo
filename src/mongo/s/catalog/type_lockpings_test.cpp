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

#include "mongo/platform/basic.h"

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace {

using namespace mongo;

TEST(Validity, MissingProcess) {
    BSONObj objNoProcess = BSON(LockpingsType::ping(Date_t::now()));
    auto lockpingsResult = LockpingsType::fromBSON(objNoProcess);
    ASSERT_NOT_OK(lockpingsResult.getStatus());
}

TEST(Validity, MissingPing) {
    BSONObj objNoPing = BSON(LockpingsType::process("host.local:27017:1352918870:16807"));
    auto lockpingsResult = LockpingsType::fromBSON(objNoPing);
    ASSERT_NOT_OK(lockpingsResult.getStatus());
}

TEST(Validity, Valid) {
    BSONObj obj = BSON(LockpingsType::process("host.local:27017:1352918870:16807")
                       << LockpingsType::ping(Date_t::fromMillisSinceEpoch(1)));
    auto lockpingsResult = LockpingsType::fromBSON(obj);
    ASSERT_OK(lockpingsResult.getStatus());

    const LockpingsType& lockPing = lockpingsResult.getValue();
    ASSERT_EQUALS(lockPing.getProcess(), "host.local:27017:1352918870:16807");
    ASSERT_EQUALS(lockPing.getPing(), Date_t::fromMillisSinceEpoch(1));
}

TEST(Validity, BadType) {
    BSONObj obj = BSON(LockpingsType::process() << 0);
    auto lockpingsResult = LockpingsType::fromBSON(obj);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, lockpingsResult.getStatus());
}

}  // unnamed namespace
