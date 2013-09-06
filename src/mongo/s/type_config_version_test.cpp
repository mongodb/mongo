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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/s/type_config_version.h"
#include "mongo/unittest/unittest.h"

/**
 * Basic tests for config version parsing.
 */

namespace {

    using std::string;
    using mongo::VersionType;
    using mongo::BSONObj;
    using mongo::BSONArray;
    using mongo::BSONObjBuilder;
    using mongo::BSONArrayBuilder;
    using mongo::OID;

    TEST(Validity, Empty) {

        //
        // Tests parsing of empty document
        //

        VersionType versionInfo;
        BSONObj emptyObj = BSONObj();

        // parses ok
        string errMsg;
        bool result = versionInfo.parseBSON(emptyObj, &errMsg);
        ASSERT_EQUALS(errMsg, "");
        ASSERT(result);

        // not valid
        result = versionInfo.isValid(&errMsg);
        ASSERT_NOT_EQUALS(errMsg, "");
        ASSERT(!result);
    }

    TEST(Validity, OldVersion) {

        //
        // Tests parsing of deprecated format
        //

        VersionType versionInfo;

        BSONObj versionDoc = BSON(VersionType::version_DEPRECATED(2));

        string errMsg;
        bool result = versionInfo.parseBSON(versionDoc, &errMsg);
        ASSERT_EQUALS(errMsg, "");
        ASSERT(result);
        ASSERT_EQUALS(versionInfo.getMinCompatibleVersion(), 2);
        ASSERT_EQUALS(versionInfo.getCurrentVersion(), 2);
        ASSERT(!versionInfo.isClusterIdSet());

        result = versionInfo.isValid(&errMsg);
        ASSERT_EQUALS(errMsg, "");
        ASSERT(result);
    }

    TEST(Validity, NewVersion) {

        //
        // Tests parsing a new-style config version
        //

        VersionType versionInfo;

        OID clusterId = OID::gen();

        BSONObjBuilder bob;
        bob << VersionType::version_DEPRECATED(3);
        bob << VersionType::minCompatibleVersion(3);
        bob << VersionType::currentVersion(4);
        bob << VersionType::clusterId(clusterId);

        BSONObj versionDoc = bob.obj();

        string errMsg;
        bool result = versionInfo.parseBSON(versionDoc, &errMsg);
        ASSERT_EQUALS(errMsg, "");
        ASSERT(result);
        ASSERT_EQUALS(versionInfo.getMinCompatibleVersion(), 3);
        ASSERT_EQUALS(versionInfo.getCurrentVersion(), 4);
        ASSERT_EQUALS(versionInfo.getClusterId(), clusterId);

        // Valid
        result = versionInfo.isValid(&errMsg);
        ASSERT_EQUALS(errMsg, "");
        ASSERT(result);
    }

    TEST(Validity, NewVersionRoundTrip) {

        //
        // Round-trip
        //

        VersionType versionInfo;

        OID clusterId = OID::gen();
        OID upgradeId = OID::gen();
        BSONObj upgradeState = BSON("a" << 1);

        BSONObjBuilder bob;
        bob << VersionType::version_DEPRECATED(3);
        bob << VersionType::minCompatibleVersion(3);
        bob << VersionType::currentVersion(4);
        bob << VersionType::clusterId(clusterId);
        bob << VersionType::upgradeId(upgradeId);
        bob << VersionType::upgradeState(upgradeState);

        BSONObj versionDoc = bob.obj();

        string errMsg;
        bool result = versionInfo.parseBSON(versionDoc, &errMsg);
        ASSERT_EQUALS(errMsg, "");
        ASSERT(result);

        result = versionInfo.parseBSON(versionInfo.toBSON(), &errMsg);
        ASSERT_EQUALS(errMsg, "");
        ASSERT(result);
        ASSERT_EQUALS(versionInfo.getMinCompatibleVersion(), 3);
        ASSERT_EQUALS(versionInfo.getCurrentVersion(), 4);
        ASSERT_EQUALS(versionInfo.getClusterId(), clusterId);
        ASSERT_EQUALS(versionInfo.getUpgradeId(), upgradeId);
        ASSERT_EQUALS(versionInfo.getUpgradeState(), upgradeState);

        // Valid
        result = versionInfo.isValid(&errMsg);
        ASSERT_EQUALS(errMsg, "");
        ASSERT(result);
    }

    TEST(Validity, NewVersionNoClusterId) {

        //
        // Tests error on parsing new format with no clusterId
        //

        VersionType versionInfo;

        BSONObjBuilder bob;
        bob << VersionType::version_DEPRECATED(3);
        bob << VersionType::minCompatibleVersion(3);
        bob << VersionType::currentVersion(4);

        BSONObj versionDoc = bob.obj();

        // Parses ok
        string errMsg;
        bool result = versionInfo.parseBSON(versionDoc, &errMsg);
        ASSERT_EQUALS(errMsg, "");
        ASSERT(result);

        // Not valid
        result = versionInfo.isValid(&errMsg);
        ASSERT_NOT_EQUALS(errMsg, "");
        ASSERT(!result);
    }

} // unnamed namespace
