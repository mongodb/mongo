// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
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
