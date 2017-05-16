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


#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_record.h"
#include "mongo/db/logical_session_record_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(LogicalSessionRecordTest, ToAndFromBSONTest) {
    // Round-trip a BSON obj
    auto lsid = LogicalSessionId::gen();
    auto lastUse = Date_t::now();
    auto oid = OID::gen();

    auto owner = BSON("userName"
                      << "Sam"
                      << "dbName"
                      << "test"
                      << "userId"
                      << oid);
    auto bson = BSON("lsid" << lsid.toBSON() << "lastUse" << lastUse << "owner" << owner);

    // Make a session record out of this
    auto res = LogicalSessionRecord::parse(bson);
    ASSERT(res.isOK());
    auto record = res.getValue();

    ASSERT_EQ(record.getLsid(), lsid);
    ASSERT_EQ(record.getLastUse(), lastUse);

    auto recordOwner = record.getSessionOwner();

    ASSERT_EQ(recordOwner.first.getUser(), "Sam");
    ASSERT_EQ(recordOwner.first.getDB(), "test");
    ASSERT(recordOwner.second);
    ASSERT_EQ(*(recordOwner.second), oid);

    // Dump back to bson, make sure we get the same thing
    auto bsonDump = record.toBSON();
    ASSERT_EQ(bsonDump.woCompare(bson), 0);
}

}  // namespace
}  // namespace mongo
