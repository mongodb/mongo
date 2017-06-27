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
#include "mongo/crypto/sha1_block.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/signed_logical_session_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

TEST(SignedLogicalSessionIdTest, ConstructWithLsid) {
    auto lsid = LogicalSessionId::gen();
    SignedLogicalSessionId slsid(lsid, boost::none, 1, SHA1Block{});
    ASSERT_EQ(slsid.getLsid(), lsid);
}

TEST(SignedLogicalSessionIdTest, FromBSONTest) {
    auto lsid = LogicalSessionId::gen();

    BSONObjBuilder b;
    b.append("lsid", lsid.toBSON());
    b.append("keyId", 4ll);
    char buffer[SHA1Block::kHashLength] = {0};
    b.appendBinData("signature", SHA1Block::kHashLength, BinDataGeneral, buffer);
    auto bson = b.done();

    auto slsid = SignedLogicalSessionId::parse(bson);
    ASSERT_EQ(slsid.getLsid(), lsid);

    // Dump back to BSON, make sure we get the same thing
    auto bsonDump = slsid.toBSON();
    ASSERT_EQ(bsonDump.woCompare(bson), 0);

    // Try parsing mal-formatted bson objs
    ASSERT_THROWS(SignedLogicalSessionId::parse(BSON("hi"
                                                     << "there")),
                  UserException);

    ASSERT_THROWS(SignedLogicalSessionId::parse(BSON("lsid"
                                                     << "not a session id!")),
                  UserException);
    ASSERT_THROWS(SignedLogicalSessionId::parse(BSON("lsid" << 14)), UserException);
}

}  // namespace
}  // namespace mongo
