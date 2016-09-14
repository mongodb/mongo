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
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << 1),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key"
                                     << "not an object"),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSONArray()),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfFieldRepeatedInKeyPattern) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1 << "field" << 1)),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1 << "otherField" << -1 << "field"
                                                           << "2dsphere")),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfKeyPatternIsNotPresent) {
    ASSERT_EQ(ErrorCodes::FailedToParse,
              validateIndexSpec(BSONObj(),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceIsNotAString) {
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << 1),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << BSONObj()),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceIsEmptyString) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns"
                                           << ""),
                                NamespaceString(),
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceDoesNotMatch) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns"
                                           << "some string"),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));

    // Verify that we reject the index specification when the "ns" field only contains the
    // collection name.
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.coll()),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, ReturnsIndexSpecWithNamespaceFilledInIfItIsNotPresent) {
    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 1),
                                    kTestNamespace,
                                    ServerGlobalParams::FeatureCompatibility::Version::k34);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 1)),
        sorted(result.getValue()));

    // Verify that the index specification we returned is still considered valid.
    ASSERT_OK(validateIndexSpec(
        result.getValue(), kTestNamespace, ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, ReturnsIndexSpecUnchangedIfNamespaceAndVersionArePresent) {
    auto result = validateIndexSpec(
        BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 1),
        kTestNamespace,
        ServerGlobalParams::FeatureCompatibility::Version::k34);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "ns"
                                        << "test.index_spec_validate"
                                        << "v"
                                        << 1)),
                      sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsNotANumber) {
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v"
                                           << "not a number"),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << BSONObj()),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsNotRepresentableAsInt) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 2.2),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << std::nan("1")),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v"
                                           << std::numeric_limits<double>::infinity()),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(
                  BSON("key" << BSON("field" << 1) << "v" << std::numeric_limits<long long>::max()),
                  kTestNamespace,
                  ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsV0) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 0),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsUnsupported) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(
                  BSON("key" << BSON("field" << 1) << "v" << 3 << "collation" << BSON("locale"
                                                                                      << "en")),
                  kTestNamespace,
                  ServerGlobalParams::FeatureCompatibility::Version::k34));

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << -3LL),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfIndexVersionIsV2AndFeatureCompatibilityVersionIs32) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 2),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k32));
}

TEST(IndexSpecValidateTest, AcceptsIndexVersionsThatAreAllowedForCreation) {
    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 1),
                                    kTestNamespace,
                                    ServerGlobalParams::FeatureCompatibility::Version::k34);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 1)),
        sorted(result.getValue()));

    result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 2LL),
                               kTestNamespace,
                               ServerGlobalParams::FeatureCompatibility::Version::k34);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 2LL)),
        sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, DefaultIndexVersionIsV1IfFeatureCompatibilityVersionIs32) {
    auto result =
        validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns()),
                          kTestNamespace,
                          ServerGlobalParams::FeatureCompatibility::Version::k32);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 1)),
        sorted(result.getValue()));

    // Verify that the index specification we returned is still considered valid.
    ASSERT_OK(validateIndexSpec(
        result.getValue(), kTestNamespace, ServerGlobalParams::FeatureCompatibility::Version::k32));
}

TEST(IndexSpecValidateTest, DefaultIndexVersionIsV2IfFeatureCompatibilityVersionIs34) {
    auto result =
        validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns()),
                          kTestNamespace,
                          ServerGlobalParams::FeatureCompatibility::Version::k34);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 2)),
        sorted(result.getValue()));

    // Verify that the index specification we returned is still considered valid.
    ASSERT_OK(validateIndexSpec(
        result.getValue(), kTestNamespace, ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, AcceptsIndexVersionV1WhenFeatureCompatibilityVersionIs34) {
    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 1),
                                    kTestNamespace,
                                    ServerGlobalParams::FeatureCompatibility::Version::k34);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 1)),
        sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfCollationIsNotAnObject) {
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "collation" << 1),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "collation"
                                           << "not an object"),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "collation" << BSONArray()),
                                kTestNamespace,
                                ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfCollationIsPresentAndVersionIsLessThanV2) {
    ASSERT_EQ(
        ErrorCodes::CannotCreateIndex,
        validateIndexSpec(BSON("key" << BSON("field" << 1) << "collation" << BSONObj() << "v" << 1),
                          kTestNamespace,
                          ServerGlobalParams::FeatureCompatibility::Version::k34));
}

TEST(IndexSpecValidateTest, AcceptsAnyObjectValueForCollation) {
    auto result = validateIndexSpec(
        BSON("key" << BSON("field" << 1) << "v" << 2 << "collation" << BSON("locale"
                                                                            << "simple")),
        kTestNamespace,
        ServerGlobalParams::FeatureCompatibility::Version::k34);
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
                          ServerGlobalParams::FeatureCompatibility::Version::k34);
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
                               ServerGlobalParams::FeatureCompatibility::Version::k34);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 2
                          << "collation"
                          << BSON("unknownCollationOption" << true))),
        sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, AcceptsIndexSpecIfCollationIsPresentAndVersionIsEqualToV2) {
    auto result = validateIndexSpec(
        BSON("key" << BSON("field" << 1) << "v" << 2 << "collation" << BSON("locale"
                                                                            << "en")),
        kTestNamespace,
        ServerGlobalParams::FeatureCompatibility::Version::k34);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 2
                          << "collation"
                          << BSON("locale"
                                  << "en"))),
        sorted(result.getValue()));
}

}  // namespace
}  // namespace mongo
