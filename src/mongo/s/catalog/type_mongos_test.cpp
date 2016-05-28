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
#include "mongo/s/catalog/type_mongos.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace {

using namespace mongo;

TEST(Validity, MissingName) {
    BSONObj obj =
        BSON(MongosType::ping(Date_t::fromMillisSinceEpoch(1)) << MongosType::uptime(100)
                                                               << MongosType::waiting(false)
                                                               << MongosType::mongoVersion("x.x.x")
                                                               << MongosType::configVersion(0));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, mongosTypeResult.getStatus());
}

TEST(Validity, MissingPing) {
    BSONObj obj = BSON(MongosType::name("localhost:27017") << MongosType::uptime(100)
                                                           << MongosType::waiting(false)
                                                           << MongosType::mongoVersion("x.x.x")
                                                           << MongosType::configVersion(0));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, mongosTypeResult.getStatus());
}

TEST(Validity, MissingUp) {
    BSONObj obj = BSON(MongosType::name("localhost:27017")
                       << MongosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::waiting(false)
                       << MongosType::mongoVersion("x.x.x")
                       << MongosType::configVersion(0));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, mongosTypeResult.getStatus());
}

TEST(Validity, MissingWaiting) {
    BSONObj obj = BSON(MongosType::name("localhost:27017")
                       << MongosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::uptime(100)
                       << MongosType::mongoVersion("x.x.x")
                       << MongosType::configVersion(0));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, mongosTypeResult.getStatus());
}

TEST(Validity, MissingMongoVersion) {
    BSONObj obj = BSON(MongosType::name("localhost:27017")
                       << MongosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::uptime(100)
                       << MongosType::waiting(false)
                       << MongosType::configVersion(0));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_OK(mongosTypeResult.getStatus());

    MongosType& mtype = mongosTypeResult.getValue();
    /**
     * Note: mongoVersion should eventually become mandatory, but is optional now
     *       for backward compatibility reasons.
     */
    ASSERT_OK(mtype.validate());
}

TEST(Validity, MissingConfigVersion) {
    BSONObj obj = BSON(MongosType::name("localhost:27017")
                       << MongosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::uptime(100)
                       << MongosType::waiting(false)
                       << MongosType::mongoVersion("x.x.x"));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_OK(mongosTypeResult.getStatus());

    MongosType& mtype = mongosTypeResult.getValue();
    /**
     * Note: configVersion should eventually become mandatory, but is optional now
     *       for backward compatibility reasons.
     */
    ASSERT_OK(mtype.validate());
}

TEST(Validity, Valid) {
    BSONObj obj = BSON(MongosType::name("localhost:27017")
                       << MongosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::uptime(100)
                       << MongosType::waiting(false)
                       << MongosType::mongoVersion("x.x.x")
                       << MongosType::configVersion(0));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    MongosType& mType = mongosTypeResult.getValue();

    ASSERT_OK(mType.validate());

    ASSERT_EQUALS(mType.getName(), "localhost:27017");
    ASSERT_EQUALS(mType.getPing(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_EQUALS(mType.getUptime(), 100);
    ASSERT_EQUALS(mType.getWaiting(), false);
    ASSERT_EQUALS(mType.getMongoVersion(), "x.x.x");
    ASSERT_EQUALS(mType.getConfigVersion(), 0);
}

TEST(Validity, BadType) {
    BSONObj obj = BSON(MongosType::name() << 0);

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::TypeMismatch, mongosTypeResult.getStatus());
}

}  // unnamed namespace
