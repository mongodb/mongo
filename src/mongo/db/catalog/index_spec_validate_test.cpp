/**
 *    Copyright 2016 MongoDB Inc.
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

#include "mongo/db/catalog/index_key_validate.h"

#include <cmath>
#include <limits>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString kTestNamespace("test", "index_spec_validate");

/**
 * Helper function used to return the fields of a BSONObj in a consistent order.
 */
BSONObj sorted(const BSONObj& obj) {
    BSONObjIteratorSorted iter(obj);
    BSONObjBuilder bob;
    while (iter.more()) {
        bob.append(iter.next());
    }
    return bob.obj();
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfKeyPatternIsNotAnObject) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << 1), kTestNamespace, featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key"
                                     << "not an object"),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSONArray()), kTestNamespace, featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfFieldRepeatedInKeyPattern) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1 << "field" << 1)),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1 << "otherField" << -1 << "field"
                                                           << "2dsphere")),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfKeyPatternIsNotPresent) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::FailedToParse,
              validateIndexSpec(BSONObj(), kTestNamespace, featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceIsNotAString) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << 1),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << BSONObj()),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceIsEmptyString) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns"
                                           << ""),
                                NamespaceString(),
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceDoesNotMatch) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns"
                                           << "some string"),
                                kTestNamespace,
                                featureCompatibility));

    // Verify that we reject the index specification when the "ns" field only contains the
    // collection name.
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.coll()),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsIndexSpecWithNamespaceFilledInIfItIsNotPresent) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(
        BSON("key" << BSON("field" << 1) << "v" << 1), kTestNamespace, featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 1)),
        sorted(result.getValue()));

    // Verify that the index specification we returned is still considered valid.
    ASSERT_OK(validateIndexSpec(result.getValue(), kTestNamespace, featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsIndexSpecUnchangedIfNamespaceAndVersionArePresent) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(
        BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 1),
        kTestNamespace,
        featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "ns"
                                        << "test.index_spec_validate"
                                        << "v"
                                        << 1)),
                      sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsNotANumber) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v"
                                           << "not a number"),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << BSONObj()),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsNotRepresentableAsInt) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 2.2),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << std::nan("1")),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v"
                                           << std::numeric_limits<double>::infinity()),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(
                  BSON("key" << BSON("field" << 1) << "v" << std::numeric_limits<long long>::max()),
                  kTestNamespace,
                  featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsV0) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 0),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsUnsupported) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(
                  BSON("key" << BSON("field" << 1) << "v" << 3 << "collation" << BSON("locale"
                                                                                      << "en")),
                  kTestNamespace,
                  featureCompatibility));

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << -3LL),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfIndexVersionIsV2AndFeatureCompatibilityVersionIs32) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k32);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 2),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, AcceptsIndexVersionV2WhenValidateFeaturesAsMasterFalse) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k32);
    featureCompatibility.validateFeaturesAsMaster.store(false);

    auto result = validateIndexSpec(
        BSON("key" << BSON("field" << 1) << "v" << 2), kTestNamespace, featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 2)),
        sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, AcceptsIndexVersionsThatAreAllowedForCreation) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(
        BSON("key" << BSON("field" << 1) << "v" << 1), kTestNamespace, featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 1)),
        sorted(result.getValue()));

    result = validateIndexSpec(
        BSON("key" << BSON("field" << 1) << "v" << 2LL), kTestNamespace, featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 2LL)),
        sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, DefaultIndexVersionIsV1IfFeatureCompatibilityVersionIs32) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k32);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result =
        validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns()),
                          kTestNamespace,
                          featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 1)),
        sorted(result.getValue()));

    // Verify that the index specification we returned is still considered valid.
    ASSERT_OK(validateIndexSpec(result.getValue(), kTestNamespace, featureCompatibility));
}

TEST(IndexSpecValidateTest,
     DefaultIndexVersionIsV1IfFeatureCompatibilityVersionIs32AndValidateFeaturesAsMasterFalse) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k32);
    featureCompatibility.validateFeaturesAsMaster.store(false);

    auto result =
        validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns()),
                          kTestNamespace,
                          featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 1)),
        sorted(result.getValue()));

    // Verify that the index specification we returned is still considered valid.
    ASSERT_OK(validateIndexSpec(result.getValue(), kTestNamespace, featureCompatibility));
}

TEST(IndexSpecValidateTest, DefaultIndexVersionIsV2IfFeatureCompatibilityVersionIs34) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result =
        validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns()),
                          kTestNamespace,
                          featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 2)),
        sorted(result.getValue()));

    // Verify that the index specification we returned is still considered valid.
    ASSERT_OK(validateIndexSpec(result.getValue(), kTestNamespace, featureCompatibility));
}

TEST(IndexSpecValidateTest, AcceptsIndexVersionV1WhenFeatureCompatibilityVersionIs34) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(
        BSON("key" << BSON("field" << 1) << "v" << 1), kTestNamespace, featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 1)),
        sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfCollationIsNotAnObject) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "collation" << 1),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "collation"
                                           << "not an object"),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "collation" << BSONArray()),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfCollationIsPresentAndVersionIsLessThanV2) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(
        ErrorCodes::CannotCreateIndex,
        validateIndexSpec(BSON("key" << BSON("field" << 1) << "collation" << BSONObj() << "v" << 1),
                          kTestNamespace,
                          featureCompatibility));
}

TEST(IndexSpecValidateTest, AcceptsAnyObjectValueForCollation) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(
        BSON("key" << BSON("field" << 1) << "v" << 2 << "collation" << BSON("locale"
                                                                            << "simple")),
        kTestNamespace,
        featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 2
                          << "collation"
                          << BSON("locale"
                                  << "simple"))),
        sorted(result.getValue()));

    result =
        validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 2 << "collation" << BSONObj()),
                          kTestNamespace,
                          featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 2
                          << "collation"
                          << BSONObj())),
        sorted(result.getValue()));

    result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 2 << "collation"
                                          << BSON("unknownCollationOption" << true)),
                               kTestNamespace,
                               featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 2
                          << "collation"
                          << BSON("unknownCollationOption" << true))),
        sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, AcceptsIndexSpecIfCollationIsPresentAndVersionIsEqualToV2) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(
        BSON("key" << BSON("field" << 1) << "v" << 2 << "collation" << BSON("locale"
                                                                            << "en")),
        kTestNamespace,
        featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 2
                          << "collation"
                          << BSON("locale"
                                  << "en"))),
        sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfUnknownFieldIsPresentInSpecV2) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result =
        validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 2 << "unknownField" << 1),
                          kTestNamespace,
                          featureCompatibility);
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption, result);
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfUnknownFieldIsPresentInSpecV1) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result =
        validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 1 << "unknownField" << 1),
                          kTestNamespace,
                          featureCompatibility);
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption, result);
}

}  // namespace
}  // namespace mongo
