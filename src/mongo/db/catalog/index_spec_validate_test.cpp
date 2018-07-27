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
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/server_options.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using index_key_validate::validateIndexSpec;
using index_key_validate::validateIdIndexSpec;
using index_key_validate::validateIndexSpecCollation;

const NamespaceString kTestNamespace("test", "index_spec_validate");
constexpr OperationContext* kDefaultOpCtx = nullptr;

/**
 * Helper class to ensure proper FCV & test commands enabled.
 */
class TestCommandFcvGuard {

public:
    TestCommandFcvGuard() {
        _prevVersion = serverGlobalParams.featureCompatibility.getVersion();
        serverGlobalParams.featureCompatibility.setVersion(
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42);
        // TODO: Remove test command enabling/disabling in SERVER-36198
        _prevEnabled = getTestCommandsEnabled();
        setTestCommandsEnabled(true);

        // TODO: Remove knob enabling/disabling in SERVER-36198.
        _prevKnobEnabled = internalQueryAllowAllPathsIndexes.load();
        internalQueryAllowAllPathsIndexes.store(true);
    }

    ~TestCommandFcvGuard() {
        serverGlobalParams.featureCompatibility.setVersion(_prevVersion);
        setTestCommandsEnabled(_prevEnabled);
        internalQueryAllowAllPathsIndexes.store(_prevKnobEnabled);
    }

private:
    bool _prevEnabled;
    bool _prevKnobEnabled;
    ServerGlobalParams::FeatureCompatibility::Version _prevVersion;
};


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
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << 1 << "name"
                                           << "indexName"),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key"
                                     << "not an object"
                                     << "name"
                                     << "indexName"),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSONArray() << "name"
                                           << "indexName"),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfFieldRepeatedInKeyPattern) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1 << "field" << 1) << "name"
                                           << "indexName"),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1 << "otherField" << -1 << "field"
                                                           << "2dsphere")
                                           << "name"
                                           << "indexName"),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfKeyPatternIsNotPresent) {
    ASSERT_EQ(ErrorCodes::FailedToParse,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("name"
                                     << "indexName"),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNameIsNotAString) {
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name" << 1),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNameIsNotPresent) {
    ASSERT_EQ(ErrorCodes::FailedToParse,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1)),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceIsNotAString) {
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "ns"
                                           << 1),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "ns"
                                           << BSONObj()),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceIsEmptyString) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "ns"
                                           << ""),
                                NamespaceString(),
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceDoesNotMatch) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "ns"
                                           << "some string"),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));

    // Verify that we reject the index specification when the "ns" field only contains the
    // collection name.
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "ns"
                                           << kTestNamespace.coll()),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsIndexSpecWithNamespaceFilledInIfItIsNotPresent) {
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 1),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
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
    ASSERT_OK(validateIndexSpec(
        kDefaultOpCtx, result.getValue(), kTestNamespace, serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsIndexSpecUnchangedIfNamespaceAndVersionArePresent) {
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "ns"
                                               << kTestNamespace.ns()
                                               << "v"
                                               << 1),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
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
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << "not a number"),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << BSONObj()),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsNotRepresentableAsInt) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << 2.2),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << std::nan("1")),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << std::numeric_limits<double>::infinity()),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << std::numeric_limits<long long>::max()),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsV0) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << 0),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsUnsupported) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << 3
                                           << "collation"
                                           << BSON("locale"
                                                   << "en")),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << -3LL),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, AcceptsIndexVersionsThatAreAllowedForCreation) {
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 1),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "ns"
                                        << kTestNamespace.ns()
                                        << "v"
                                        << 1)),
                      sorted(result.getValue()));

    result = validateIndexSpec(kDefaultOpCtx,
                               BSON("key" << BSON("field" << 1) << "name"
                                          << "indexName"
                                          << "v"
                                          << 2LL),
                               kTestNamespace,
                               serverGlobalParams.featureCompatibility);
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

TEST(IndexSpecValidateTest, DefaultIndexVersionIsV2) {
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "ns"
                                               << kTestNamespace.ns()),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
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
    ASSERT_OK(validateIndexSpec(
        kDefaultOpCtx, result.getValue(), kTestNamespace, serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, AcceptsIndexVersionV1) {
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 1),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
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
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation"
                                           << 1),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation"
                                           << "not an object"),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation"
                                           << BSONArray()),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfCollationIsEmpty) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation"
                                           << BSONObj()),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfCollationIsPresentAndVersionIsLessThanV2) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(kDefaultOpCtx,
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation"
                                           << BSON("locale"
                                                   << "simple")
                                           << "v"
                                           << 1),
                                kTestNamespace,
                                serverGlobalParams.featureCompatibility));
}

TEST(IndexSpecValidateTest, AcceptsAnyNonEmptyObjectValueForCollation) {
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 2
                                               << "collation"
                                               << BSON("locale"
                                                       << "simple")),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
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

    result = validateIndexSpec(kDefaultOpCtx,
                               BSON("key" << BSON("field" << 1) << "name"
                                          << "indexName"
                                          << "v"
                                          << 2
                                          << "collation"
                                          << BSON("unknownCollationOption" << true)),
                               kTestNamespace,
                               serverGlobalParams.featureCompatibility);
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
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 2
                                               << "collation"
                                               << BSON("locale"
                                                       << "en")),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
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
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 2
                                               << "unknownField"
                                               << 1),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption, result);
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfUnknownFieldIsPresentInSpecV1) {
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v"
                                               << 1
                                               << "unknownField"
                                               << 1),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
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
    auto opCtx = serviceContext.makeOperationContext();

    const CollatorInterface* defaultCollator = nullptr;

    auto result = validateIndexSpecCollation(opCtx.get(),
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
    auto opCtx = serviceContext.makeOperationContext();

    const CollatorInterface* defaultCollator = nullptr;

    auto result = validateIndexSpecCollation(opCtx.get(),
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
    auto opCtx = serviceContext.makeOperationContext();

    const CollatorInterfaceMock defaultCollator(CollatorInterfaceMock::MockType::kReverseString);

    auto result = validateIndexSpecCollation(opCtx.get(),
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

TEST(IndexSpecPartialFilterTest, FailsIfPartialFilterIsNotAnObject) {
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "partialFilterExpression"
                                               << 1),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(IndexSpecPartialFilterTest, FailsIfPartialFilterContainsBannedFeature) {
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "partialFilterExpression"
                                               << BSON("$jsonSchema" << BSONObj())),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_EQ(result.getStatus(), ErrorCodes::QueryFeatureNotAllowed);
}

TEST(IndexSpecPartialFilterTest, AcceptsValidPartialFilterExpression) {
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "partialFilterExpression"
                                               << BSON("a" << 1)),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_OK(result.getStatus());
}

TEST(IndexSpecAllPaths, SucceedsWithInclusion) {
    TestCommandFcvGuard guard;
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "starPathsTempName"
                                               << BSON("a" << 1 << "b" << 1)),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_OK(result.getStatus());
}

TEST(IndexSpecAllPaths, SucceedsWithExclusion) {
    TestCommandFcvGuard guard;
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "starPathsTempName"
                                               << BSON("a" << 0 << "b" << 0)),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_OK(result.getStatus());
}

TEST(IndexSpecAllPaths, SucceedsWithExclusionIncludingId) {
    TestCommandFcvGuard guard;
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "starPathsTempName"
                                               << BSON("_id" << 1 << "a" << 0 << "b" << 0)),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_OK(result.getStatus());
}

TEST(IndexSpecAllPaths, SucceedsWithInclusionExcludingId) {
    TestCommandFcvGuard guard;
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "starPathsTempName"
                                               << BSON("_id" << 0 << "a" << 1 << "b" << 1)),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_OK(result.getStatus());
}

TEST(IndexSpecAllPaths, FailsWithInclusionExcludingIdSubfield) {
    TestCommandFcvGuard guard;
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "starPathsTempName"
                                               << BSON("_id.field" << 0 << "a" << 1 << "b" << 1)),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_EQ(result.getStatus().code(), 40179);
}

TEST(IndexSpecAllPaths, FailsWithExclusionIncludingIdSubfield) {
    TestCommandFcvGuard guard;
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "starPathsTempName"
                                               << BSON("_id.field" << 1 << "a" << 0 << "b" << 0)),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_EQ(result.getStatus().code(), 40178);
}

TEST(IndexSpecAllPaths, FailsWithImproperFeatureCompatabilityVersion) {
    TestCommandFcvGuard guard;
    serverGlobalParams.featureCompatibility.setVersion(
        ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo42);
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::CannotCreateIndex);
}

TEST(IndexSpecAllPaths, FailsWithMixedProjection) {
    TestCommandFcvGuard guard;
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "starPathsTempName"
                                               << BSON("a" << 1 << "b" << 0)),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_EQ(result.getStatus().code(), 40178);
}

TEST(IndexSpecAllPaths, FailsWithComputedFieldsInProjection) {
    TestCommandFcvGuard guard;
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "starPathsTempName"
                                               << BSON("a" << 1 << "b"
                                                           << "string")),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST(IndexSpecAllPaths, FailsWhenProjectionPluginNotAllPaths) {
    TestCommandFcvGuard guard;
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("a" << 1) << "name"
                                               << "indexName"
                                               << "starPathsTempName"
                                               << BSON("a" << 1)),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
}

TEST(IndexSpecAllPaths, FailsWhenProjectionIsNotAnObject) {
    TestCommandFcvGuard guard;
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "starPathsTempName"
                                               << 4),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::TypeMismatch);
}

TEST(IndexSpecAllPaths, FailsWithEmptyProjection) {
    TestCommandFcvGuard guard;
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "starPathsTempName"
                                               << BSONObj()),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST(IndexSpecAllPaths, FailsWhenInclusionWithSubpath) {
    TestCommandFcvGuard guard;
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("a.$**" << 1) << "name"
                                               << "indexName"
                                               << "starPathsTempName"
                                               << BSON("a" << 1)),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST(IndexSpecAllPaths, FailsWhenExclusionWithSubpath) {
    TestCommandFcvGuard guard;
    auto result = validateIndexSpec(kDefaultOpCtx,
                                    BSON("key" << BSON("a.$**" << 1) << "name"
                                               << "indexName"
                                               << "starPathsTempName"
                                               << BSON("b" << 0)),
                                    kTestNamespace,
                                    serverGlobalParams.featureCompatibility);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

}  // namespace
}  // namespace mongo
