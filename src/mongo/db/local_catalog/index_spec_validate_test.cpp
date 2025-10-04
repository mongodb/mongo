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
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/local_catalog/index_key_validate.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <cmath>
#include <limits>
#include <memory>

namespace mongo {
namespace {

using index_key_validate::validateIdIndexSpec;
using index_key_validate::validateIndexSpec;
using index_key_validate::validateIndexSpecCollation;

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

namespace index_spec_validate_test {

class IndexSpecValidateTest : public ServiceContextMongoDTest {};

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfKeyPatternIsNotAnObject) {
    auto opCtx = makeOperationContext();

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << 1 << "name"
                                           << "indexName")));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << "not an object"
                                           << "name"
                                           << "indexName")));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSONArray() << "name"
                                           << "indexName")));
}

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfFieldRepeatedInKeyPattern) {
    auto opCtx = makeOperationContext();

    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1 << "field" << 1) << "name"
                                           << "indexName")));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1 << "otherField" << -1 << "field"
                                                           << "2dsphere")
                                           << "name"
                                           << "indexName")));
}

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfKeyPatternIsNotPresent) {
    auto opCtx = makeOperationContext();

    ASSERT_EQ(ErrorCodes::FailedToParse,
              validateIndexSpec(opCtx.get(), BSON("name" << "indexName")));
}

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfNameIsNotAString) {
    auto opCtx = makeOperationContext();

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(opCtx.get(), BSON("key" << BSON("field" << 1) << "name" << 1)));
}

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfNameIsNotPresent) {
    auto opCtx = makeOperationContext();

    ASSERT_EQ(ErrorCodes::FailedToParse,
              validateIndexSpec(opCtx.get(), BSON("key" << BSON("field" << 1))));
}

TEST_F(IndexSpecValidateTest, ReturnsIndexSpecUnchangedIfVersionIsPresent) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v" << 1));
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "v" << 1)),
                      sorted(result.getValue()));
}

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsNotANumber) {
    auto opCtx = makeOperationContext();

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v"
                                           << "not a number")));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v" << BSONObj())));
}

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsNotRepresentableAsInt) {
    auto opCtx = makeOperationContext();

    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v" << 2.2)));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v" << std::nan("1"))));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v" << std::numeric_limits<double>::infinity())));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v" << std::numeric_limits<long long>::max())));
}

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsV0) {
    auto opCtx = makeOperationContext();

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v" << 0)));
}

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsUnsupported) {
    auto opCtx = makeOperationContext();

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v" << 3 << "collation" << BSON("locale" << "en"))));

    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "v" << -3LL)));
}

TEST_F(IndexSpecValidateTest, AcceptsIndexVersionsThatAreAllowedForCreation) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v" << 1));
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "v" << 1)),
                      sorted(result.getValue()));

    result = validateIndexSpec(opCtx.get(),
                               BSON("key" << BSON("field" << 1) << "name"
                                          << "indexName"
                                          << "v" << 2LL));
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "v" << 2LL)),
                      sorted(result.getValue()));
}

TEST_F(IndexSpecValidateTest, DefaultIndexVersionIsV2) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"));
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "v" << 2)),
                      sorted(result.getValue()));

    // Verify that the index specification we returned is still considered valid.
    ASSERT_OK(validateIndexSpec(opCtx.get(), result.getValue()));
}

TEST_F(IndexSpecValidateTest, AcceptsIndexVersionV1) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v" << 1));
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "v" << 1)),
                      sorted(result.getValue()));
}

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfCollationIsNotAnObject) {
    auto opCtx = makeOperationContext();

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation" << 1)));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation"
                                           << "not an object")));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation" << BSONArray())));
}

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfCollationIsEmpty) {
    auto opCtx = makeOperationContext();

    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(opCtx.get(),
                                BSON("key" << BSON("field" << 1) << "name"
                                           << "indexName"
                                           << "collation" << BSONObj())));
}

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfCollationIsPresentAndVersionIsLessThanV2) {
    auto opCtx = makeOperationContext();

    ASSERT_EQ(
        ErrorCodes::CannotCreateIndex,
        validateIndexSpec(opCtx.get(),
                          BSON("key" << BSON("field" << 1) << "name"
                                     << "indexName"
                                     << "collation" << BSON("locale" << "simple") << "v" << 1)));
}

TEST_F(IndexSpecValidateTest, AcceptsAnyNonEmptyObjectValueForCollation) {
    auto opCtx = makeOperationContext();

    auto result =
        validateIndexSpec(opCtx.get(),
                          BSON("key" << BSON("field" << 1) << "name"
                                     << "indexName"
                                     << "v" << 2 << "collation" << BSON("locale" << "simple")));
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "v" << 2 << "collation" << BSON("locale" << "simple"))),
                      sorted(result.getValue()));

    result = validateIndexSpec(opCtx.get(),
                               BSON("key" << BSON("field" << 1) << "name"
                                          << "indexName"
                                          << "v" << 2 << "collation"
                                          << BSON("unknownCollationOption" << true)));
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "name"
                          << "indexName"
                          << "v" << 2 << "collation" << BSON("unknownCollationOption" << true))),
        sorted(result.getValue()));
}

TEST_F(IndexSpecValidateTest, AcceptsIndexSpecIfCollationIsPresentAndVersionIsEqualToV2) {
    auto opCtx = makeOperationContext();

    auto result =
        validateIndexSpec(opCtx.get(),
                          BSON("key" << BSON("field" << 1) << "name"
                                     << "indexName"
                                     << "v" << 2 << "collation" << BSON("locale" << "en")));
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "v" << 2 << "collation" << BSON("locale" << "en"))),
                      sorted(result.getValue()));
}

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfUnknownFieldIsPresentInSpecV2) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v" << 2 << "unknownField" << 1));
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption, result);
}

TEST_F(IndexSpecValidateTest, ReturnsAnErrorIfUnknownFieldIsPresentInSpecV1) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "v" << 1 << "unknownField" << 1));
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption, result);
}

TEST_F(IndexSpecValidateTest, DisallowSpecifyingBothUniqueAndPrepareUnique) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("a" << 1) << "name"
                                               << "indexName"
                                               << "unique" << true << "prepareUnique" << true));
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::CannotCreateIndex);
}

}  // namespace index_spec_validate_test

namespace id_index_spec_validate_test {

class IdIndexSpecValidateTest : public ServiceContextMongoDTest {};

TEST_F(IdIndexSpecValidateTest, ReturnsAnErrorIfKeyPatternIsIncorrectForIdIndex) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIdIndexSpec(BSON("key" << BSON("_id" << -1) << "name"
                                             << IndexConstants::kIdIndexName << "v" << 2)));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIdIndexSpec(BSON("key" << BSON("a" << 1) << "name"
                                             << IndexConstants::kIdIndexName << "v" << 2)));
}

TEST_F(IdIndexSpecValidateTest, ReturnsOKStatusIfKeyPatternCorrectForIdIndex) {
    ASSERT_OK(validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name"
                                             << "anyname"
                                             << "v" << 2)));
}

TEST_F(IdIndexSpecValidateTest, ReturnsAnErrorIfFieldNotAllowedForIdIndex) {
    ASSERT_EQ(
        ErrorCodes::InvalidIndexSpecificationOption,
        validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName
                                       << "v" << 2 << "background" << false)));
    ASSERT_EQ(
        ErrorCodes::InvalidIndexSpecificationOption,
        validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName
                                       << "v" << 2 << "unique" << true)));
    ASSERT_EQ(ErrorCodes::InvalidIndexSpecificationOption,
              validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name"
                                             << IndexConstants::kIdIndexName << "v" << 2
                                             << "partialFilterExpression" << BSON("a" << 5))));
    ASSERT_EQ(
        ErrorCodes::InvalidIndexSpecificationOption,
        validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName
                                       << "v" << 2 << "sparse" << false)));
    ASSERT_EQ(
        ErrorCodes::InvalidIndexSpecificationOption,
        validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName
                                       << "v" << 2 << "expireAfterSeconds" << 3600)));
    ASSERT_EQ(
        ErrorCodes::InvalidIndexSpecificationOption,
        validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName
                                       << "v" << 2 << "storageEngine" << BSONObj())));
}

TEST_F(IdIndexSpecValidateTest, ReturnsOKStatusIfAllFieldsAllowedForIdIndex) {
    ASSERT_OK(
        validateIdIndexSpec(BSON("key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName
                                       << "v" << 2 << "collation" << BSON("locale" << "simple"))));
}

}  // namespace id_index_spec_validate_test

namespace index_spec_collation_validate_test {

class IndexSpecCollationValidateTest : public ServiceContextMongoDTest {
protected:
    IndexSpecCollationValidateTest() {
        CollatorFactoryInterface::set(getServiceContext(), std::make_unique<CollatorFactoryMock>());
    }
};

TEST_F(IndexSpecCollationValidateTest, FillsInFullCollationSpec) {
    auto opCtx = makeOperationContext();

    const CollatorInterface* defaultCollator = nullptr;

    auto result = validateIndexSpecCollation(opCtx.get(),
                                             BSON("key" << BSON("field" << 1) << "name"
                                                        << "indexName"
                                                        << "v" << 2 << "collation"
                                                        << BSON("locale" << "mock_reverse_string")),
                                             defaultCollator);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "name"
                          << "indexName"
                          << "v" << 2 << "collation"
                          << BSON("locale"
                                  << "mock_reverse_string"
                                  << "caseLevel" << false << "caseFirst"
                                  << "off"
                                  << "strength" << 3 << "numericOrdering" << false << "alternate"
                                  << "non-ignorable"
                                  << "maxVariable"
                                  << "punct"
                                  << "normalization" << false << "backwards" << false << "version"
                                  << "mock_version"))),
        sorted(result.getValue()));
}

TEST_F(IndexSpecCollationValidateTest, RemovesCollationFieldIfSimple) {
    auto opCtx = makeOperationContext();

    const CollatorInterface* defaultCollator = nullptr;

    auto result = validateIndexSpecCollation(opCtx.get(),
                                             BSON("key" << BSON("field" << 1) << "name"
                                                        << "indexName"
                                                        << "v" << 2 << "collation"
                                                        << BSON("locale" << "simple")),
                                             defaultCollator);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "name"
                                        << "indexName"
                                        << "v" << 2)),
                      sorted(result.getValue()));
}

TEST_F(IndexSpecCollationValidateTest, FillsInCollationFieldWithCollectionDefaultIfNotPresent) {
    auto opCtx = makeOperationContext();

    const CollatorInterfaceMock defaultCollator(CollatorInterfaceMock::MockType::kReverseString);

    auto result = validateIndexSpecCollation(opCtx.get(),
                                             BSON("key" << BSON("field" << 1) << "name"
                                                        << "indexName"
                                                        << "v" << 2),
                                             &defaultCollator);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "name"
                          << "indexName"
                          << "v" << 2 << "collation"
                          << BSON("locale"
                                  << "mock_reverse_string"
                                  << "caseLevel" << false << "caseFirst"
                                  << "off"
                                  << "strength" << 3 << "numericOrdering" << false << "alternate"
                                  << "non-ignorable"
                                  << "maxVariable"
                                  << "punct"
                                  << "normalization" << false << "backwards" << false << "version"
                                  << "mock_version"))),
        sorted(result.getValue()));
}

}  // namespace index_spec_collation_validate_test

namespace index_spec_partial_filter_test {

class IndexSpecPartialFilterTest : public ServiceContextMongoDTest {};

TEST_F(IndexSpecPartialFilterTest, FailsIfPartialFilterIsNotAnObject) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "partialFilterExpression" << 1));
    ASSERT_EQ(result.getStatus(), ErrorCodes::TypeMismatch);
}

TEST_F(IndexSpecPartialFilterTest, FailsIfPartialFilterContainsBannedFeature) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "partialFilterExpression"
                                               << BSON("$jsonSchema" << BSONObj())));
    ASSERT_EQ(result.getStatus(), ErrorCodes::QueryFeatureNotAllowed);
}

TEST_F(IndexSpecPartialFilterTest, AcceptsValidPartialFilterExpression) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("field" << 1) << "name"
                                               << "indexName"
                                               << "partialFilterExpression" << BSON("a" << 1)));
    ASSERT_OK(result.getStatus());
}

}  // namespace index_spec_partial_filter_test

namespace index_spec_wildcard_test {

class IndexSpecWildcardTest : public ServiceContextMongoDTest {};

TEST_F(IndexSpecWildcardTest, SucceedsWithInclusion) {
    auto opCtx = makeOperationContext();

    auto result =
        validateIndexSpec(opCtx.get(),
                          BSON("key" << BSON("$**" << 1) << "name"
                                     << "indexName"
                                     << "wildcardProjection" << BSON("a" << 1 << "b" << 1)));
    ASSERT_OK(result.getStatus());
}

TEST_F(IndexSpecWildcardTest, SucceedsWithExclusion) {
    auto opCtx = makeOperationContext();

    auto result =
        validateIndexSpec(opCtx.get(),
                          BSON("key" << BSON("$**" << 1) << "name"
                                     << "indexName"
                                     << "wildcardProjection" << BSON("a" << 0 << "b" << 0)));
    ASSERT_OK(result.getStatus());
}

TEST_F(IndexSpecWildcardTest, SucceedsWithExclusionIncludingId) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "wildcardProjection"
                                               << BSON("_id" << 1 << "a" << 0 << "b" << 0)));
    ASSERT_OK(result.getStatus());
}

TEST_F(IndexSpecWildcardTest, SucceedsWithInclusionExcludingId) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "wildcardProjection"
                                               << BSON("_id" << 0 << "a" << 1 << "b" << 1)));
    ASSERT_OK(result.getStatus());
}

TEST_F(IndexSpecWildcardTest, FailsWithInclusionExcludingIdSubfield) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "wildcardProjection"
                                               << BSON("_id.field" << 0 << "a" << 1 << "b" << 1)));
    ASSERT_EQ(result.getStatus().code(), 31253);
}

TEST_F(IndexSpecWildcardTest, FailsWithExclusionIncludingIdSubfield) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "wildcardProjection"
                                               << BSON("_id.field" << 1 << "a" << 0 << "b" << 0)));
    ASSERT_EQ(result.getStatus().code(), 31254);
}

TEST_F(IndexSpecWildcardTest, FailsWithMixedProjection) {
    auto opCtx = makeOperationContext();

    auto result =
        validateIndexSpec(opCtx.get(),
                          BSON("key" << BSON("$**" << 1) << "name"
                                     << "indexName"
                                     << "wildcardProjection" << BSON("a" << 1 << "b" << 0)));
    ASSERT_EQ(result.getStatus().code(), 31254);
}

TEST_F(IndexSpecWildcardTest, FailsWithComputedFieldsInProjection) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "wildcardProjection"
                                               << BSON("a" << 1 << "b"
                                                           << "string")));
    ASSERT_EQ(result.getStatus().code(), 51271);
}

TEST_F(IndexSpecWildcardTest, FailsWhenProjectionPluginNotWildcard) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("a" << 1) << "name"
                                               << "indexName"
                                               << "wildcardProjection" << BSON("a" << 1)));
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
}

TEST_F(IndexSpecWildcardTest, FailsWhenProjectionIsNotAnObject) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "wildcardProjection" << 4));
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::TypeMismatch);
}

TEST_F(IndexSpecWildcardTest, FailsWithEmptyProjection) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("$**" << 1) << "name"
                                               << "indexName"
                                               << "wildcardProjection" << BSONObj()));
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST_F(IndexSpecWildcardTest, FailsWhenInclusionWithSubpath) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("a.$**" << 1) << "name"
                                               << "indexName"
                                               << "wildcardProjection" << BSON("a" << 1)));
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST_F(IndexSpecWildcardTest, FailsWhenExclusionWithSubpath) {
    auto opCtx = makeOperationContext();

    auto result = validateIndexSpec(opCtx.get(),
                                    BSON("key" << BSON("a.$**" << 1) << "name"
                                               << "indexName"
                                               << "wildcardProjection" << BSON("b" << 0)));
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
}

}  // namespace index_spec_wildcard_test
}  // namespace
}  // namespace mongo
