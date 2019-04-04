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
#include "merizo/s/catalog/type_config_version.h"
#include "merizo/unittest/unittest.h"

/**
 * Basic tests for config version parsing.
 */

namespace {

using namespace merizo;

TEST(Validity, Empty) {
    //
    // Tests parsing of empty document
    //

    BSONObj emptyObj = BSONObj();
    auto versionResult = VersionType::fromBSON(emptyObj);
    ASSERT_NOT_OK(versionResult.getStatus());
}

TEST(Validity, NewVersion) {
    //
    // Tests parsing a new-style config version
    //

    OID clusterId = OID::gen();

    BSONObjBuilder bob;
    bob << VersionType::minCompatibleVersion(3);
    bob << VersionType::currentVersion(4);
    bob << VersionType::clusterId(clusterId);

    BSONObj versionDoc = bob.obj();

    auto versionResult = VersionType::fromBSON(versionDoc);
    ASSERT_OK(versionResult.getStatus());

    VersionType& versionInfo = versionResult.getValue();

    ASSERT_EQUALS(versionInfo.getMinCompatibleVersion(), 3);
    ASSERT_EQUALS(versionInfo.getCurrentVersion(), 4);
    ASSERT_EQUALS(versionInfo.getClusterId(), clusterId);

    ASSERT_OK(versionInfo.validate());
}

TEST(Validity, NewVersionRoundTrip) {
    //
    // Round-trip
    //

    OID clusterId = OID::gen();
    OID upgradeId = OID::gen();
    BSONObj upgradeState = BSON("a" << 1);

    BSONObjBuilder bob;
    bob << VersionType::minCompatibleVersion(3);
    bob << VersionType::currentVersion(4);
    bob << VersionType::clusterId(clusterId);
    bob << VersionType::upgradeId(upgradeId);
    bob << VersionType::upgradeState(upgradeState);

    BSONObj versionDoc = bob.obj();

    auto versionResult = VersionType::fromBSON(versionDoc);
    ASSERT_OK(versionResult.getStatus());

    VersionType& versionInfo = versionResult.getValue();

    ASSERT_EQUALS(versionInfo.getMinCompatibleVersion(), 3);
    ASSERT_EQUALS(versionInfo.getCurrentVersion(), 4);
    ASSERT_EQUALS(versionInfo.getClusterId(), clusterId);
    ASSERT_EQUALS(versionInfo.getUpgradeId(), upgradeId);
    ASSERT_BSONOBJ_EQ(versionInfo.getUpgradeState(), upgradeState);

    ASSERT_OK(versionInfo.validate());
}

TEST(Validity, NewVersionNoClusterId) {
    //
    // Tests error on parsing new format with no clusterId
    //

    BSONObjBuilder bob;
    bob << VersionType::minCompatibleVersion(3);
    bob << VersionType::currentVersion(4);

    BSONObj versionDoc = bob.obj();

    auto versionResult = VersionType::fromBSON(versionDoc);
    ASSERT_EQ(ErrorCodes::NoSuchKey, versionResult.getStatus());
}

TEST(Excludes, Empty) {
    //
    // Tests basic empty range
    //

    VersionType versionInfo;
    versionInfo.setExcludingMerizoVersions({});

    // Make sure nothing is included
    ASSERT(!isInMerizoVersionRanges("1.2.3", versionInfo.getExcludingMerizoVersions()));
    ASSERT(!isInMerizoVersionRanges("1.2.3-pre", versionInfo.getExcludingMerizoVersions()));
    ASSERT(!isInMerizoVersionRanges("1.2.3-rc0", versionInfo.getExcludingMerizoVersions()));
}

TEST(Excludes, SinglePointRange) {
    //
    // Tests single string range
    //

    VersionType versionInfo;
    MerizoVersionRange vr;
    vr.minVersion = "1.2.3";
    versionInfo.setExcludingMerizoVersions({vr});

    ASSERT(isInMerizoVersionRanges("1.2.3", versionInfo.getExcludingMerizoVersions()));

    ASSERT(!isInMerizoVersionRanges("1.2.2-rc0", versionInfo.getExcludingMerizoVersions()));
    ASSERT(!isInMerizoVersionRanges("1.2.2", versionInfo.getExcludingMerizoVersions()));

    ASSERT(isInMerizoVersionRanges("1.2.3-pre", versionInfo.getExcludingMerizoVersions()));
    ASSERT(isInMerizoVersionRanges("1.2.3-rc0", versionInfo.getExcludingMerizoVersions()));

    ASSERT(!isInMerizoVersionRanges("1.2.4-rc0", versionInfo.getExcludingMerizoVersions()));
    ASSERT(!isInMerizoVersionRanges("1.2.4", versionInfo.getExcludingMerizoVersions()));
}

TEST(Excludes, BetweenRange) {
    //
    // Tests range with two endpoints
    //

    VersionType versionInfo;
    MerizoVersionRange vr;
    vr.minVersion = "7.8.9";
    vr.maxVersion = "10.11.12";
    versionInfo.setExcludingMerizoVersions({vr});

    ASSERT(isInMerizoVersionRanges("7.8.9", versionInfo.getExcludingMerizoVersions()));
    ASSERT(isInMerizoVersionRanges("10.11.12", versionInfo.getExcludingMerizoVersions()));

    // Before
    ASSERT(!isInMerizoVersionRanges("7.8.8-rc0", versionInfo.getExcludingMerizoVersions()));
    ASSERT(!isInMerizoVersionRanges("7.8.8", versionInfo.getExcludingMerizoVersions()));

    // Boundary
    ASSERT(isInMerizoVersionRanges("7.8.9-pre", versionInfo.getExcludingMerizoVersions()));
    ASSERT(isInMerizoVersionRanges("7.8.9-rc0", versionInfo.getExcludingMerizoVersions()));

    ASSERT(isInMerizoVersionRanges("7.8.10-rc0", versionInfo.getExcludingMerizoVersions()));
    ASSERT(isInMerizoVersionRanges("7.8.10", versionInfo.getExcludingMerizoVersions()));

    // Between
    ASSERT(isInMerizoVersionRanges("8.9.10", versionInfo.getExcludingMerizoVersions()));
    ASSERT(isInMerizoVersionRanges("9.10.11", versionInfo.getExcludingMerizoVersions()));

    // Boundary
    ASSERT(isInMerizoVersionRanges("10.11.11-rc0", versionInfo.getExcludingMerizoVersions()));
    ASSERT(isInMerizoVersionRanges("10.11.11", versionInfo.getExcludingMerizoVersions()));

    ASSERT(isInMerizoVersionRanges("10.11.12-pre", versionInfo.getExcludingMerizoVersions()));
    ASSERT(isInMerizoVersionRanges("10.11.12-rc0", versionInfo.getExcludingMerizoVersions()));

    // After
    ASSERT(!isInMerizoVersionRanges("10.11.13-rc0", versionInfo.getExcludingMerizoVersions()));
    ASSERT(!isInMerizoVersionRanges("10.11.13", versionInfo.getExcludingMerizoVersions()));
}

TEST(Excludes, WeirdRange) {
    //
    // Tests range with rc/pre endpoints
    //

    VersionType versionInfo;
    MerizoVersionRange vr;
    vr.minVersion = "7.8.9-rc0";
    vr.maxVersion = "10.11.12-pre";
    versionInfo.setExcludingMerizoVersions({vr});

    // Near endpoints
    ASSERT(isInMerizoVersionRanges("7.8.9", versionInfo.getExcludingMerizoVersions()));
    ASSERT(!isInMerizoVersionRanges("10.11.12", versionInfo.getExcludingMerizoVersions()));

    // Before
    ASSERT(!isInMerizoVersionRanges("7.8.8-rc0", versionInfo.getExcludingMerizoVersions()));
    ASSERT(!isInMerizoVersionRanges("7.8.8", versionInfo.getExcludingMerizoVersions()));

    // Boundary
    ASSERT(!isInMerizoVersionRanges("7.8.9-pre", versionInfo.getExcludingMerizoVersions()));
    ASSERT(isInMerizoVersionRanges("7.8.9-rc0", versionInfo.getExcludingMerizoVersions()));

    ASSERT(isInMerizoVersionRanges("7.8.10-rc0", versionInfo.getExcludingMerizoVersions()));
    ASSERT(isInMerizoVersionRanges("7.8.10", versionInfo.getExcludingMerizoVersions()));

    // Between
    ASSERT(isInMerizoVersionRanges("8.9.10", versionInfo.getExcludingMerizoVersions()));
    ASSERT(isInMerizoVersionRanges("9.10.11", versionInfo.getExcludingMerizoVersions()));

    // Boundary
    ASSERT(isInMerizoVersionRanges("10.11.11-rc0", versionInfo.getExcludingMerizoVersions()));
    ASSERT(isInMerizoVersionRanges("10.11.11", versionInfo.getExcludingMerizoVersions()));

    ASSERT(isInMerizoVersionRanges("10.11.12-pre", versionInfo.getExcludingMerizoVersions()));
    ASSERT(!isInMerizoVersionRanges("10.11.12-rc0", versionInfo.getExcludingMerizoVersions()));

    // After
    ASSERT(!isInMerizoVersionRanges("10.11.13-rc0", versionInfo.getExcludingMerizoVersions()));
    ASSERT(!isInMerizoVersionRanges("10.11.13", versionInfo.getExcludingMerizoVersions()));
}

TEST(Excludes, BadRangeArray) {
    //
    // Tests range with bad array
    //

    BSONArrayBuilder bab;
    bab << BSON_ARRAY(""
                      << "1.2.3");  // empty bound
    BSONArray includeArr = bab.arr();

    auto versionInfoResult = VersionType::fromBSON(BSON(
        VersionType::minCompatibleVersion(3) << VersionType::currentVersion(4)
                                             << VersionType::clusterId(OID::gen())
                                             << VersionType::excludingMerizoVersions(includeArr)));
    ASSERT_EQ(ErrorCodes::FailedToParse, versionInfoResult.getStatus());
}

}  // unnamed namespace
