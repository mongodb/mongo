/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

TEST(LogicalSessionIdTest, ToAndFromStringTest) {
    // Round-trip a UUID string
    auto uuid = UUID::gen();
    auto uuidString = uuid.toString();

    auto res = LogicalSessionId::parse(uuidString);
    ASSERT(res.isOK());

    auto lsidString = res.getValue().toString();
    ASSERT_EQUALS(uuidString, lsidString);

    // Test with a bad string
    res = LogicalSessionId::parse("not a session id!");
    ASSERT(!res.isOK());
}

TEST(LogicalSessionIdTest, FromBSONTest) {
    auto uuid = UUID::gen();
    auto bson = BSON("id" << uuid.toBSON());

    auto lsid = LogicalSessionId::parse(bson);
    ASSERT_EQUALS(lsid.toString(), uuid.toString());

    // Dump back to BSON, make sure we get the same thing
    auto bsonDump = lsid.toBSON();
    ASSERT_EQ(bsonDump.woCompare(bson), 0);

    // Try parsing mal-formatted bson objs
    ASSERT_THROWS(LogicalSessionId::parse(BSON("hi"
                                               << "there")),
                  UserException);

    // TODO: use these and add more once there is bindata
    ASSERT_THROWS(LogicalSessionId::parse(BSON("id"
                                               << "not a session id!")),
                  UserException);
    ASSERT_THROWS(LogicalSessionId::parse(BSON("id" << 14)), UserException);
}

}  // namespace
}  // namespace mongo
