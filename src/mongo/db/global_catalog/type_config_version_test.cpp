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

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/type_config_version_gen.h"
#include "mongo/stdx/type_traits.h"
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
    ASSERT_THROWS(VersionType::parse(emptyObj, IDLParserContext("VersionType")),
                  AssertionException);
}

TEST(Validity, NewVersion) {
    //
    // Tests parsing a new-style config version
    //

    OID clusterId = OID::gen();

    auto versionDoc = BSON(VersionType::kClusterIdFieldName << clusterId);

    auto versionResult = VersionType::parse(versionDoc, IDLParserContext("VersionType"));

    ASSERT_EQUALS(versionResult.getClusterId(), clusterId);
}

TEST(Validity, NewVersionRoundTrip) {
    //
    // Round-trip
    //

    OID clusterId = OID::gen();

    auto versionDoc = BSON(VersionType::kClusterIdFieldName << clusterId);

    auto versionResult = VersionType::parse(versionDoc, IDLParserContext("VersionType"));

    ASSERT_EQUALS(versionResult.getClusterId(), clusterId);

    auto newVersionDoc = versionResult.toBSON();

    auto newVersionResult = VersionType::parse(newVersionDoc, IDLParserContext("VersionType"));

    ASSERT_EQUALS(newVersionResult.getClusterId(), clusterId);
}

TEST(Validity, NewVersionNoClusterId) {
    //
    // Tests error on parsing new format with no clusterId
    //

    auto versionDoc = BSON("test" << "test_value");

    ASSERT_THROWS(VersionType::parse(versionDoc, IDLParserContext("VersionType")),
                  AssertionException);
}

TEST(Validity, NewVersionWrongClusterId) {
    //
    // Tests error on parsing new format with wrong type of clusterId
    //

    auto versionDoc = BSON(VersionType::kClusterIdFieldName << "not really OID");

    ASSERT_THROWS(VersionType::parse(versionDoc, IDLParserContext("VersionType")),
                  AssertionException);
}

TEST(Validity, NewVersionWithId) {
    //
    // Tests parsing a new-style config version with _id field
    //

    OID clusterId = OID::gen();

    BSONObjBuilder bob;
    bob.append("clusterId", clusterId);
    bob.append("_id", 1);
    BSONObj versionDoc = bob.obj();
    auto versionResult = VersionType::parse(versionDoc, IDLParserContext("VersionType"));

    ASSERT_EQUALS(versionResult.getClusterId(), clusterId);
}
}  // unnamed namespace
