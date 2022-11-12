/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
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
    bob << VersionType::clusterId(clusterId);

    BSONObj versionDoc = bob.obj();

    auto versionResult = VersionType::fromBSON(versionDoc);
    ASSERT_OK(versionResult.getStatus());

    VersionType& versionInfo = versionResult.getValue();

    ASSERT_EQUALS(versionInfo.getClusterId(), clusterId);

    ASSERT_OK(versionInfo.validate());
}

TEST(Validity, NewVersionRoundTrip) {
    //
    // Round-trip
    //

    OID clusterId = OID::gen();

    BSONObjBuilder bob;
    bob << VersionType::clusterId(clusterId);

    BSONObj versionDoc = bob.obj();

    auto versionResult = VersionType::fromBSON(versionDoc);
    ASSERT_OK(versionResult.getStatus());

    VersionType& versionInfo = versionResult.getValue();

    ASSERT_EQUALS(versionInfo.getClusterId(), clusterId);

    ASSERT_OK(versionInfo.validate());
}

TEST(Validity, NewVersionNoClusterId) {
    //
    // Tests error on parsing new format with no clusterId
    //

    BSONObjBuilder bob;
    BSONObj versionDoc = bob.obj();

    auto versionResult = VersionType::fromBSON(versionDoc);
    ASSERT_EQ(ErrorCodes::NoSuchKey, versionResult.getStatus());
}

}  // unnamed namespace
