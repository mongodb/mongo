/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "merizo/platform/basic.h"

#include "merizo/base/status_with.h"
#include "merizo/db/jsobj.h"
#include "merizo/s/catalog/type_merizos.h"
#include "merizo/unittest/unittest.h"
#include "merizo/util/time_support.h"

namespace {

using namespace merizo;

TEST(Validity, MissingName) {
    BSONObj obj = BSON(MerizosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MerizosType::uptime(100)
                       << MerizosType::waiting(false)
                       << MerizosType::merizoVersion("x.x.x")
                       << MerizosType::configVersion(0)
                       << MerizosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto merizosTypeResult = MerizosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, merizosTypeResult.getStatus());
}

TEST(Validity, MissingPing) {
    BSONObj obj = BSON(MerizosType::name("localhost:27017")
                       << MerizosType::uptime(100)
                       << MerizosType::waiting(false)
                       << MerizosType::merizoVersion("x.x.x")
                       << MerizosType::configVersion(0)
                       << MerizosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto merizosTypeResult = MerizosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, merizosTypeResult.getStatus());
}

TEST(Validity, MissingUp) {
    BSONObj obj = BSON(MerizosType::name("localhost:27017")
                       << MerizosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MerizosType::waiting(false)
                       << MerizosType::merizoVersion("x.x.x")
                       << MerizosType::configVersion(0)
                       << MerizosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto merizosTypeResult = MerizosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, merizosTypeResult.getStatus());
}

TEST(Validity, MissingWaiting) {
    BSONObj obj = BSON(MerizosType::name("localhost:27017")
                       << MerizosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MerizosType::uptime(100)
                       << MerizosType::merizoVersion("x.x.x")
                       << MerizosType::configVersion(0)
                       << MerizosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto merizosTypeResult = MerizosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, merizosTypeResult.getStatus());
}

TEST(Validity, MissingMerizoVersion) {
    BSONObj obj = BSON(MerizosType::name("localhost:27017")
                       << MerizosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MerizosType::uptime(100)
                       << MerizosType::waiting(false)
                       << MerizosType::configVersion(0)
                       << MerizosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto merizosTypeResult = MerizosType::fromBSON(obj);
    ASSERT_OK(merizosTypeResult.getStatus());

    MerizosType& mtype = merizosTypeResult.getValue();
    /**
     * Note: merizoVersion should eventually become mandatory, but is optional now
     *       for backward compatibility reasons.
     */
    ASSERT_OK(mtype.validate());
}

TEST(Validity, MissingConfigVersion) {
    BSONObj obj = BSON(MerizosType::name("localhost:27017")
                       << MerizosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MerizosType::uptime(100)
                       << MerizosType::waiting(false)
                       << MerizosType::merizoVersion("x.x.x")
                       << MerizosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto merizosTypeResult = MerizosType::fromBSON(obj);
    ASSERT_OK(merizosTypeResult.getStatus());

    MerizosType& mtype = merizosTypeResult.getValue();
    /**
     * Note: configVersion should eventually become mandatory, but is optional now
     *       for backward compatibility reasons.
     */
    ASSERT_OK(mtype.validate());
}

TEST(Validity, MissingAdvisoryHostFQDNs) {
    BSONObj obj = BSON(MerizosType::name("localhost:27017")
                       << MerizosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MerizosType::uptime(100)
                       << MerizosType::waiting(false)
                       << MerizosType::merizoVersion("x.x.x")
                       << MerizosType::configVersion(0));

    auto merizosTypeResult = MerizosType::fromBSON(obj);
    ASSERT_OK(merizosTypeResult.getStatus());

    MerizosType& mType = merizosTypeResult.getValue();
    // advisoryHostFQDNs is optional
    ASSERT_OK(mType.validate());
}

TEST(Validity, EmptyAdvisoryHostFQDNs) {
    BSONObj obj = BSON(MerizosType::name("localhost:27017")
                       << MerizosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MerizosType::uptime(100)
                       << MerizosType::waiting(false)
                       << MerizosType::merizoVersion("x.x.x")
                       << MerizosType::configVersion(0)
                       << MerizosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto merizosTypeResult = MerizosType::fromBSON(obj);
    ASSERT_OK(merizosTypeResult.getStatus());

    MerizosType& mType = merizosTypeResult.getValue();
    ASSERT_OK(mType.validate());

    ASSERT_EQUALS(mType.getAdvisoryHostFQDNs().size(), 0UL);
}

TEST(Validity, BadTypeAdvisoryHostFQDNs) {
    BSONObj obj = BSON(MerizosType::name("localhost:27017")
                       << MerizosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MerizosType::uptime(100)
                       << MerizosType::waiting(false)
                       << MerizosType::merizoVersion("x.x.x")
                       << MerizosType::configVersion(0)
                       << MerizosType::advisoryHostFQDNs(BSON_ARRAY("foo" << 0 << "baz")));

    auto merizosTypeResult = MerizosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::TypeMismatch, merizosTypeResult.getStatus());
}

TEST(Validity, Valid) {
    BSONObj obj = BSON(MerizosType::name("localhost:27017")
                       << MerizosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MerizosType::uptime(100)
                       << MerizosType::waiting(false)
                       << MerizosType::merizoVersion("x.x.x")
                       << MerizosType::configVersion(0)
                       << MerizosType::advisoryHostFQDNs(BSON_ARRAY("foo"
                                                                   << "bar"
                                                                   << "baz")));

    auto merizosTypeResult = MerizosType::fromBSON(obj);
    ASSERT_OK(merizosTypeResult.getStatus());

    MerizosType& mType = merizosTypeResult.getValue();
    ASSERT_OK(mType.validate());

    ASSERT_EQUALS(mType.getName(), "localhost:27017");
    ASSERT_EQUALS(mType.getPing(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_EQUALS(mType.getUptime(), 100);
    ASSERT_EQUALS(mType.getWaiting(), false);
    ASSERT_EQUALS(mType.getMerizoVersion(), "x.x.x");
    ASSERT_EQUALS(mType.getConfigVersion(), 0);
    ASSERT_EQUALS(mType.getAdvisoryHostFQDNs().size(), 3UL);
    ASSERT_EQUALS(mType.getAdvisoryHostFQDNs()[0], "foo");
    ASSERT_EQUALS(mType.getAdvisoryHostFQDNs()[1], "bar");
    ASSERT_EQUALS(mType.getAdvisoryHostFQDNs()[2], "baz");
}

TEST(Validity, BadType) {
    BSONObj obj = BSON(MerizosType::name() << 0);

    auto merizosTypeResult = MerizosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::TypeMismatch, merizosTypeResult.getStatus());
}

}  // unnamed namespace
