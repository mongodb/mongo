/*
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/stdx/chrono.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::rpc;
using mongo::unittest::assertGet;

TrackingMetadata checkParse(const BSONObj& metadata) {
    return assertGet(TrackingMetadata::readFromMetadata(metadata));
}

const auto kOperId = OID{"541b1a00e8a23afa832b218e"};
const auto kOperName = "testCmd";
const auto kParentOperId = "541b1a00e8a23afa832b2016";

TEST(TrackingMetadata, ReadFromMetadata) {
    {
        auto metadata = checkParse(BSON(
            "tracking_info" << BSON("operId" << kOperId << "operName" << kOperName << "parentOperId"
                                             << kParentOperId)));
        ASSERT_EQ(*metadata.getOperId(), kOperId);
        ASSERT_EQ(*metadata.getParentOperId(), kParentOperId);
        ASSERT_EQ(*metadata.getOperName(), kOperName);
    }
}

void checkParseFails(const BSONObj& metadata, ErrorCodes::Error error) {
    auto tm = TrackingMetadata::readFromMetadata(metadata);
    ASSERT_NOT_OK(tm.getStatus());
    ASSERT_EQ(tm.getStatus(), error);
}

TEST(TrackingMetadata, ReadFromInvalidMetadata) {
    { checkParseFails(BSON("tracking_info" << 1), ErrorCodes::TypeMismatch); }
    { checkParseFails(BSON("tracking_info" << BSON("o" << 111)), ErrorCodes::NoSuchKey); }
    { checkParseFails(BSON("tracking_info" << BSON("operId" << 111)), ErrorCodes::TypeMismatch); }
    { checkParseFails(BSON("tracking_info" << BSON("operId" << kOperId)), ErrorCodes::NoSuchKey); }
    {
        checkParseFails(BSON("tracking_info" << BSON("operId" << kOperId << "operName" << 111)),
                        ErrorCodes::TypeMismatch);
    }
    {
        checkParseFails(BSON("tracking_info" << BSON("operId" << kOperId << "operName" << kOperName
                                                              << "parentOperId"
                                                              << 111)),
                        ErrorCodes::TypeMismatch);
    }
}

TEST(TrackingMetadata, Roundtrip1) {
    BSONObjBuilder bob;
    TrackingMetadata tmBefore{kOperId, kOperName, kParentOperId};
    tmBefore.writeToMetadata(&bob);
    auto tmAfter = TrackingMetadata::readFromMetadata(bob.obj());
    ASSERT_OK(tmAfter.getStatus());
    auto metadata = tmAfter.getValue();
    ASSERT_EQ(*metadata.getOperId(), kOperId);
    ASSERT_EQ(*metadata.getParentOperId(), kParentOperId);
    ASSERT_EQ(*metadata.getOperName(), kOperName);
}

TEST(TrackingMetadata, Roundtrip2) {
    BSONObjBuilder bob;
    TrackingMetadata tmBefore{kOperId, kOperName};
    tmBefore.writeToMetadata(&bob);
    auto tmAfter = TrackingMetadata::readFromMetadata(bob.obj());
    ASSERT_OK(tmAfter.getStatus());
    auto metadata = tmAfter.getValue();
    ASSERT_EQ(*metadata.getOperId(), kOperId);
    ASSERT_EQ(*metadata.getOperName(), kOperName);
}

}  // namespace
