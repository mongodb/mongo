// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/public/extension_command_static_properties_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo::extension {
namespace {

using MongoExtensionCommandAllowedOnSecondaryEnum::kAlways;
using MongoExtensionCommandAllowedOnSecondaryEnum::kNever;
using MongoExtensionCommandClusterRoleEnum::kBoth;
using MongoExtensionCommandClusterRoleEnum::kShard;
using MongoExtensionCommandReadWriteTypeEnum::kCommand;
using MongoExtensionCommandReadWriteTypeEnum::kWrite;
using MongoExtensionPrivilegeActionEnum::kCreateSearchIndexes;
using MongoExtensionPrivilegeResourcePatternEnum::kNamespace;

TEST(ExtensionCommandStaticPropertiesTest, DefaultsAreApplied) {
    // requiredPrivileges is a required field, so every valid document supplies it.
    const auto properties =
        MongoExtensionCommandStaticProperties::parse(BSON("requiredPrivileges" << BSONArray()));

    ASSERT_EQ(properties.getClusterRole(), kBoth);
    ASSERT_EQ(properties.getReadWriteType(), kCommand);
    ASSERT_EQ(properties.getAllowedOnSecondary(), kNever);
    ASSERT_FALSE(properties.getAdminOnly());
    ASSERT_FALSE(properties.getSupportsWriteConcern());
    ASSERT_FALSE(properties.getIsSubjectToIngressAdmissionControl());
}

TEST(ExtensionCommandStaticPropertiesTest, EmptyRequiredPrivilegesMeansNoPrivileges) {
    const auto properties =
        MongoExtensionCommandStaticProperties::parse(BSON("requiredPrivileges" << BSONArray()));

    ASSERT_TRUE(properties.getRequiredPrivileges().empty());
}

TEST(ExtensionCommandStaticPropertiesTest, EmptyDeclarationIsRejected) {
    // Unlike the aggregation-stage properties, an empty declaration is not valid here because
    // requiredPrivileges has no default.
    IDLParserContext ctxt("properties");
    ASSERT_THROWS_CODE_AND_WHAT(
        MongoExtensionCommandStaticProperties::parse(BSONObj(), ctxt),
        AssertionException,
        ErrorCodes::IDLFailedToParse,
        "BSON field 'properties.requiredPrivileges' is missing but a required field");
}

TEST(ExtensionCommandStaticPropertiesTest, InvalidEnumValuesAreRejected) {
    IDLParserContext ctxt("properties");

    ASSERT_THROWS_CODE(
        MongoExtensionCommandStaticProperties::parse(
            BSON("clusterRole" << "notARole" << "requiredPrivileges" << BSONArray()), ctxt),
        DBException,
        ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(
        MongoExtensionCommandStaticProperties::parse(
            BSON("readWriteType" << "notAType" << "requiredPrivileges" << BSONArray()), ctxt),
        DBException,
        ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(
        MongoExtensionCommandStaticProperties::parse(
            BSON("allowedOnSecondary" << "sometimes" << "requiredPrivileges" << BSONArray()), ctxt),
        DBException,
        ErrorCodes::BadValue);
}

TEST(ExtensionCommandStaticPropertiesTest, NonDefaultValuesParse) {
    const auto properties = MongoExtensionCommandStaticProperties::parse(
        BSON("clusterRole" << "shard"
                           << "readWriteType" << "write"
                           << "allowedOnSecondary" << "always"
                           << "adminOnly" << true << "supportsWriteConcern" << true
                           << "isSubjectToIngressAdmissionControl" << true << "requiredPrivileges"
                           << BSONArray()));

    ASSERT_EQ(properties.getClusterRole(), kShard);
    ASSERT_EQ(properties.getReadWriteType(), kWrite);
    ASSERT_EQ(properties.getAllowedOnSecondary(), kAlways);
    ASSERT_TRUE(properties.getAdminOnly());
    ASSERT_TRUE(properties.getSupportsWriteConcern());
    ASSERT_TRUE(properties.getIsSubjectToIngressAdmissionControl());
}

TEST(ExtensionCommandStaticPropertiesTest, RequiredPrivilegesParseAndRoundTrip) {
    // Fields are written in IDL declaration order so the serialized form compares equal.
    const auto doc = BSON(
        "clusterRole" << "shard"
                      << "readWriteType" << "write"
                      << "allowedOnSecondary" << "always"
                      << "adminOnly" << true << "supportsWriteConcern" << true
                      << "isSubjectToIngressAdmissionControl" << true << "requiredPrivileges"
                      << BSON_ARRAY(BSON("resourcePattern"
                                         << "namespace"
                                         << "actions"
                                         << BSON_ARRAY(BSON("action" << "createSearchIndexes")))));

    const auto properties = MongoExtensionCommandStaticProperties::parse(doc);

    const auto& privileges = properties.getRequiredPrivileges();
    ASSERT_EQ(privileges.size(), 1u);
    ASSERT_EQ(privileges[0].getResourcePattern(), kNamespace);
    ASSERT_EQ(privileges[0].getActions().size(), 1u);
    ASSERT_EQ(privileges[0].getActions()[0].getAction(), kCreateSearchIndexes);

    ASSERT_BSONOBJ_EQ(doc, properties.toBSON());
}

TEST(ExtensionCommandStaticPropertiesTest, EmptyActionsIsRejected) {
    // Tests the underlying required privileges validation logic which also applies to
    // extension-defined agg stages.
    IDLParserContext ctxt("properties");
    ASSERT_THROWS_CODE(MongoExtensionCommandStaticProperties::parse(
                           BSON("requiredPrivileges"
                                << BSON_ARRAY(BSON("resourcePattern" << "namespace"
                                                                     << "actions" << BSONArray()))),
                           ctxt),
                       AssertionException,
                       ErrorCodes::ExtensionError);
}

TEST(ExtensionCommandStaticPropertiesTest, UnknownFieldsAreIgnored) {
    const auto properties = MongoExtensionCommandStaticProperties::parse(
        BSON("requiredPrivileges" << BSONArray() << "notARealField" << 1));

    ASSERT_EQ(properties.getClusterRole(), kBoth);
}

}  // namespace
}  // namespace mongo::extension
