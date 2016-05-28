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
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/unittest/unittest.h"

/**
 * Basic tests for config version parsing.
 */

namespace {

using namespace mongo;

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
    ASSERT_EQUALS(versionInfo.getUpgradeState(), upgradeState);

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
    versionInfo.setExcludingMongoVersions({});

    // Make sure nothing is included
    ASSERT(!isInMongoVersionRanges("1.2.3", versionInfo.getExcludingMongoVersions()));
    ASSERT(!isInMongoVersionRanges("1.2.3-pre", versionInfo.getExcludingMongoVersions()));
    ASSERT(!isInMongoVersionRanges("1.2.3-rc0", versionInfo.getExcludingMongoVersions()));
}

TEST(Excludes, SinglePointRange) {
    //
    // Tests single string range
    //

    VersionType versionInfo;
    MongoVersionRange vr;
    vr.minVersion = "1.2.3";
    versionInfo.setExcludingMongoVersions({vr});

    ASSERT(isInMongoVersionRanges("1.2.3", versionInfo.getExcludingMongoVersions()));

    ASSERT(!isInMongoVersionRanges("1.2.2-rc0", versionInfo.getExcludingMongoVersions()));
    ASSERT(!isInMongoVersionRanges("1.2.2", versionInfo.getExcludingMongoVersions()));

    ASSERT(isInMongoVersionRanges("1.2.3-pre", versionInfo.getExcludingMongoVersions()));
    ASSERT(isInMongoVersionRanges("1.2.3-rc0", versionInfo.getExcludingMongoVersions()));

    ASSERT(!isInMongoVersionRanges("1.2.4-rc0", versionInfo.getExcludingMongoVersions()));
    ASSERT(!isInMongoVersionRanges("1.2.4", versionInfo.getExcludingMongoVersions()));
}

TEST(Excludes, BetweenRange) {
    //
    // Tests range with two endpoints
    //

    VersionType versionInfo;
    MongoVersionRange vr;
    vr.minVersion = "7.8.9";
    vr.maxVersion = "10.11.12";
    versionInfo.setExcludingMongoVersions({vr});

    ASSERT(isInMongoVersionRanges("7.8.9", versionInfo.getExcludingMongoVersions()));
    ASSERT(isInMongoVersionRanges("10.11.12", versionInfo.getExcludingMongoVersions()));

    // Before
    ASSERT(!isInMongoVersionRanges("7.8.8-rc0", versionInfo.getExcludingMongoVersions()));
    ASSERT(!isInMongoVersionRanges("7.8.8", versionInfo.getExcludingMongoVersions()));

    // Boundary
    ASSERT(isInMongoVersionRanges("7.8.9-pre", versionInfo.getExcludingMongoVersions()));
    ASSERT(isInMongoVersionRanges("7.8.9-rc0", versionInfo.getExcludingMongoVersions()));

    ASSERT(isInMongoVersionRanges("7.8.10-rc0", versionInfo.getExcludingMongoVersions()));
    ASSERT(isInMongoVersionRanges("7.8.10", versionInfo.getExcludingMongoVersions()));

    // Between
    ASSERT(isInMongoVersionRanges("8.9.10", versionInfo.getExcludingMongoVersions()));
    ASSERT(isInMongoVersionRanges("9.10.11", versionInfo.getExcludingMongoVersions()));

    // Boundary
    ASSERT(isInMongoVersionRanges("10.11.11-rc0", versionInfo.getExcludingMongoVersions()));
    ASSERT(isInMongoVersionRanges("10.11.11", versionInfo.getExcludingMongoVersions()));

    ASSERT(isInMongoVersionRanges("10.11.12-pre", versionInfo.getExcludingMongoVersions()));
    ASSERT(isInMongoVersionRanges("10.11.12-rc0", versionInfo.getExcludingMongoVersions()));

    // After
    ASSERT(!isInMongoVersionRanges("10.11.13-rc0", versionInfo.getExcludingMongoVersions()));
    ASSERT(!isInMongoVersionRanges("10.11.13", versionInfo.getExcludingMongoVersions()));
}

TEST(Excludes, WeirdRange) {
    //
    // Tests range with rc/pre endpoints
    //

    VersionType versionInfo;
    MongoVersionRange vr;
    vr.minVersion = "7.8.9-rc0";
    vr.maxVersion = "10.11.12-pre";
    versionInfo.setExcludingMongoVersions({vr});

    // Near endpoints
    ASSERT(isInMongoVersionRanges("7.8.9", versionInfo.getExcludingMongoVersions()));
    ASSERT(!isInMongoVersionRanges("10.11.12", versionInfo.getExcludingMongoVersions()));

    // Before
    ASSERT(!isInMongoVersionRanges("7.8.8-rc0", versionInfo.getExcludingMongoVersions()));
    ASSERT(!isInMongoVersionRanges("7.8.8", versionInfo.getExcludingMongoVersions()));

    // Boundary
    ASSERT(!isInMongoVersionRanges("7.8.9-pre", versionInfo.getExcludingMongoVersions()));
    ASSERT(isInMongoVersionRanges("7.8.9-rc0", versionInfo.getExcludingMongoVersions()));

    ASSERT(isInMongoVersionRanges("7.8.10-rc0", versionInfo.getExcludingMongoVersions()));
    ASSERT(isInMongoVersionRanges("7.8.10", versionInfo.getExcludingMongoVersions()));

    // Between
    ASSERT(isInMongoVersionRanges("8.9.10", versionInfo.getExcludingMongoVersions()));
    ASSERT(isInMongoVersionRanges("9.10.11", versionInfo.getExcludingMongoVersions()));

    // Boundary
    ASSERT(isInMongoVersionRanges("10.11.11-rc0", versionInfo.getExcludingMongoVersions()));
    ASSERT(isInMongoVersionRanges("10.11.11", versionInfo.getExcludingMongoVersions()));

    ASSERT(isInMongoVersionRanges("10.11.12-pre", versionInfo.getExcludingMongoVersions()));
    ASSERT(!isInMongoVersionRanges("10.11.12-rc0", versionInfo.getExcludingMongoVersions()));

    // After
    ASSERT(!isInMongoVersionRanges("10.11.13-rc0", versionInfo.getExcludingMongoVersions()));
    ASSERT(!isInMongoVersionRanges("10.11.13", versionInfo.getExcludingMongoVersions()));
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
                                             << VersionType::excludingMongoVersions(includeArr)));
    ASSERT_EQ(ErrorCodes::FailedToParse, versionInfoResult.getStatus());
}

}  // unnamed namespace
