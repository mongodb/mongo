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
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using index_key_validate::validateIndexSpec;
using index_key_validate::validateIdIndexSpec;
using index_key_validate::validateIndexSpecCollation;

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
              validateIndexSpec(BSON("key" << 1 << "name"
                                           << "indexName"),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key"
                                     << "not an object"
                                     << "name"
                                     << "indexName"),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSONArray() << "name"
                                           << "indexName"),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfFieldRepeatedInKeyPattern) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1 << "field" << 1) << "name"
                                           << "indexName"),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1 << "otherField" << -1 << "field"
                                                           << "2dsphere")
                                           << "name"
                                           << "indexName"),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfKeyPatternIsNotPresent) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::FailedToParse,
              validateIndexSpec(BSON("name"
                                     << "indexName"),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNameIsNotAString) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name" << 1),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNameIsNotPresent) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(
        ErrorCodes::FailedToParse,
        validateIndexSpec(BSON("key" << BSON("field" << 1)), kTestNamespace, featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceIsNotAString) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "ns"
                                           << 1),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "ns"
                                           << BSONObj()),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceIsEmptyString) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "ns"
                                           << ""),
                                NamespaceString(),
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceDoesNotMatch) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "ns"
                                           << "some string"),
                                kTestNamespace,
                                featureCompatibility));

    // Verify that we reject the index specification when the "ns" field only contains the
    // collection name.
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "ns"
                                           << kTestNamespace.coll()),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsIndexSpecWithNamespaceFilledInIfItIsNotPresent) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 1),
                                    kTestNamespace,
                                    featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 1)),
                      sorted(result.getValue()));

    // Verify that the index specification we returned is still considered valid.
    ASSERT_OK(validateIndexSpec(result.getValue(), kTestNamespace, featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsIndexSpecUnchangedIfNamespaceAndVersionArePresent) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "ns"
                                               << kTestNamespace.ns()
                                               << "v"
                                               << 1),
                                    kTestNamespace,
                                    featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
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
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << "not a number"),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << BSONObj()),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsNotRepresentableAsInt) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << 2.2),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << std::nan("1")),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << std::numeric_limits<double>::infinity()),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << std::numeric_limits<long long>::max()),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsV0) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << 0),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsUnsupported) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << 3
                                           << "collation"
                                           << BSON("locale"
                                                   << "en")),
                                kTestNamespace,
                                featureCompatibility));

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << -3LL),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfIndexVersionIsV2AndFeatureCompatibilityVersionIs32) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k32);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << 2),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, AcceptsIndexVersionV2WhenValidateFeaturesAsMasterFalse) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k32);
    featureCompatibility.validateFeaturesAsMaster.store(false);

    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 2),
                                    kTestNamespace,
                                    featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 2)),
                      sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, AcceptsIndexVersionsThatAreAllowedForCreation) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 1),
                                    kTestNamespace,
                                    featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 1)),
                      sorted(result.getValue()));

    result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                          << "indexName"
                                          << "v"
                                          << 2LL),
                               kTestNamespace,
                               featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 2LL)),
                      sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, DefaultIndexVersionIsV1IfFeatureCompatibilityVersionIs32) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k32);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "ns"
                                               << kTestNamespace.ns()),
                                    kTestNamespace,
                                    featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 1)),
                      sorted(result.getValue()));

    // Verify that the index specification we returned is still considered valid.
    ASSERT_OK(validateIndexSpec(result.getValue(), kTestNamespace, featureCompatibility));
}

TEST(IndexSpecValidateTest,
     DefaultIndexVersionIsV1IfFeatureCompatibilityVersionIs32AndValidateFeaturesAsMasterFalse) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k32);
    featureCompatibility.validateFeaturesAsMaster.store(false);

    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "ns"
                                               << kTestNamespace.ns()),
                                    kTestNamespace,
                                    featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 1)),
                      sorted(result.getValue()));

    // Verify that the index specification we returned is still considered valid.
    ASSERT_OK(validateIndexSpec(result.getValue(), kTestNamespace, featureCompatibility));
}

TEST(IndexSpecValidateTest, DefaultIndexVersionIsV2IfFeatureCompatibilityVersionIs34) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "ns"
                                               << kTestNamespace.ns()),
                                    kTestNamespace,
                                    featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 2)),
                      sorted(result.getValue()));

    // Verify that the index specification we returned is still considered valid.
    ASSERT_OK(validateIndexSpec(result.getValue(), kTestNamespace, featureCompatibility));
}

TEST(IndexSpecValidateTest, AcceptsIndexVersionV1WhenFeatureCompatibilityVersionIs34) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 1),
                                    kTestNamespace,
                                    featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 1)),
                      sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfCollationIsNotAnObject) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation"
                                           << 1),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation"
                                           << "not an object"),
                                kTestNamespace,
                                featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation"
                                           << BSONArray()),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfCollationIsEmpty) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation"
                                           << BSONObj()),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfCollationIsPresentAndVersionIsLessThanV2) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation"
                                           << BSON("locale"
                                                   << "simple")
                                           << "v"
                                           << 1),
                                kTestNamespace,
                                featureCompatibility));
}

TEST(IndexSpecValidateTest, AcceptsAnyNonEmptyObjectValueForCollation) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 2
                                               << "collation"
                                               << BSON("locale"
                                                       << "simple")),
                                    kTestNamespace,
                                    featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 2
                                        << "collation"
                                        << BSON("locale"
                                                << "simple"))),
                      sorted(result.getValue()));

    result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                          << "indexName"
                                          << "v"
                                          << 2
                                          << "collation"
                                          << BSON("unknownCollationOption" << true)),
                               kTestNamespace,
                               featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 2
                                        << "collation"
                                        << BSON("unknownCollationOption" << true))),
                      sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, AcceptsIndexSpecIfCollationIsPresentAndVersionIsEqualToV2) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 2
                                               << "collation"
                                               << BSON("locale"
                                                       << "en")),
                                    kTestNamespace,
                                    featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 2
                                        << "collation"
                                        << BSON("locale"
                                                << "en"))),
                      sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfUnknownFieldIsPresentInSpecV2) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 2
                                               << "unknownField"
                                               << 1),
                                    kTestNamespace,
                                    featureCompatibility);
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption, result);
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfUnknownFieldIsPresentInSpecV1) {
    ServerGlobalParams::FeatureCompatibility featureCompatibility;
    featureCompatibility.version.store(ServerGlobalParams::FeatureCompatibility::Version::k34);
    featureCompatibility.validateFeaturesAsMaster.store(true);

    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 1
                                               << "unknownField"
                                               << 1),
                                    kTestNamespace,
                                    featureCompatibility);
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption, result);
}

TEST(IdIndexSpecValidateTest, ReturnsAnErrorIfKeyPatternIsIncorrectForIdIndex) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIdIndexSpec(BSON("key" << BSON("_id" << -1) << "name"
                                             << "_id_"
                                             << "ns"
                                             << kTestNamespace.ns()
                                             << "v"
                                             << 2)));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIdIndexSpec(BSON("key" << BSON("a" << 1) << "name"
                                             << "_id_"
                                             << "ns"
                                             << kTestNamespace.ns()
                                             << "v"
                                             << 2)));
}

TEST(IdIndexSpecValidateTest, ReturnsOKStatusIfKeyPatternCorrectForIdIndex) {
    ASSERT_OK(validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name"
                                             << "anyname"
                                             << "ns"
                                             << kTestNamespace.ns()
                                             << "v"
                                             << 2)));
}

TEST(IdIndexSpecValidateTest, ReturnsAnErrorIfFieldNotAllowedForIdIndex) {
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption,
              validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name"
                                             << "_id_"
                                             << "ns"
                                             << kTestNamespace.ns()
                                             << "v"
                                             << 2
                                             << "background"
                                             << false)));
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption,
              validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name"
                                             << "_id_"
                                             << "ns"
                                             << kTestNamespace.ns()
                                             << "v"
                                             << 2
                                             << "unique"
                                             << true)));
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption,
              validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name"
                                             << "_id_"
                                             << "ns"
                                             << kTestNamespace.ns()
                                             << "v"
                                             << 2
                                             << "partialFilterExpression"
                                             << BSON("a" << 5))));
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption,
              validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name"
                                             << "_id_"
                                             << "ns"
                                             << kTestNamespace.ns()
                                             << "v"
                                             << 2
                                             << "sparse"
                                             << false)));
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption,
              validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name"
                                             << "_id_"
                                             << "ns"
                                             << kTestNamespace.ns()
                                             << "v"
                                             << 2
                                             << "expireAfterSeconds"
                                             << 3600)));
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption,
              validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name"
                                             << "_id_"
                                             << "ns"
                                             << kTestNamespace.ns()
                                             << "v"
                                             << 2
                                             << "storageEngine"
                                             << BSONObj())));
}

TEST(IdIndexSpecValidateTest, ReturnsOKStatusIfAllFieldsAllowedForIdIndex) {
    ASSERT_OK(validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name"
                                             << "_id_"
                                             << "ns"
                                             << kTestNamespace.ns()
                                             << "v"
                                             << 2
                                             << "collation"
                                             << BSON("locale"
                                                     << "simple"))));
}

TEST(IndexSpecCollationValidateTest, FillsInFullCollationSpec) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    const CollatorInterface* defaultCollator = nullptr;

    auto result = validateIndexSpecCollation(txn.get(),
                                             BSON("key" << BSON("field" << 1) << "name"
                                                        << "indexName"
                                                        << "ns"
                                                        << kTestNamespace.ns()
                                                        << "v"
                                                        << 2
                                                        << "collation"
                                                        << BSON("locale"
                                                                << "mock_reverse_string")),
                                             defaultCollator);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 2
                                        << "collation"
                                        << BSON("locale"
                                                << "mock_reverse_string"
                                                << "caseLevel"
                                                << false
                                                << "caseFirst"
                                                << "off"
                                                << "strength"
                                                << 3
                                                << "numericOrdering"
                                                << false
                                                << "alternate"
                                                << "non-ignorable"
                                                << "maxVariable"
                                                << "punct"
                                                << "normalization"
                                                << false
                                                << "backwards"
                                                << false
                                                << "version"
                                                << "mock_version"))),
                      sorted(result.getValue()));
}

TEST(IndexSpecCollationValidateTest, RemovesCollationFieldIfSimple) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    const CollatorInterface* defaultCollator = nullptr;

    auto result = validateIndexSpecCollation(txn.get(),
                                             BSON("key" << BSON("field" << 1) << "name"
                                                        << "indexName"
                                                        << "ns"
                                                        << kTestNamespace.ns()
                                                        << "v"
                                                        << 2
                                                        << "collation"
                                                        << BSON("locale"
                                                                << "simple")),
                                             defaultCollator);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 2)),
                      sorted(result.getValue()));
}

TEST(IndexSpecCollationValidateTest, FillsInCollationFieldWithCollectionDefaultIfNotPresent) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    const CollatorInterfaceMock defaultCollator(CollatorInterfaceMock::MockType::kReverseString);

    auto result = validateIndexSpecCollation(txn.get(),
                                             BSON("key" << BSON("field" << 1) << "name"
                                                        << "indexName"
                                                        << "ns"
                                                        << kTestNamespace.ns()
                                                        << "v"
                                                        << 2),
                                             &defaultCollator);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 2
                                        << "collation"
                                        << BSON("locale"
                                                << "mock_reverse_string"
                                                << "caseLevel"
                                                << false
                                                << "caseFirst"
                                                << "off"
                                                << "strength"
                                                << 3
                                                << "numericOrdering"
                                                << false
                                                << "alternate"
                                                << "non-ignorable"
                                                << "maxVariable"
                                                << "punct"
                                                << "normalization"
                                                << false
                                                << "backwards"
                                                << false
                                                << "version"
                                                << "mock_version"))),
                      sorted(result.getValue()));
}

}  // namespace
}  // namespace mongo
