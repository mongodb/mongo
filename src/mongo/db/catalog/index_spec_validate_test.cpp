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
    ASSERT_EQ(ErrorCodes::TypeMismatch, validateIndexSpec(BSON("key" << 1), kTestNamespace));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key"
                                     << "not an object"),
                                kTestNamespace));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSONArray()), kTestNamespace));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfFieldRepeatedInKeyPattern) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1 << "field" << 1)), kTestNamespace));
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1 << "otherField" << -1 << "field"
                                                           << "2dsphere")),
                                kTestNamespace));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfKeyPatternIsNotPresent) {
    ASSERT_EQ(ErrorCodes::FailedToParse, validateIndexSpec(BSONObj(), kTestNamespace));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceIsNotAString) {
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << 1), kTestNamespace));
    ASSERT_EQ(
        ErrorCodes::TypeMismatch,
        validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << BSONObj()), kTestNamespace));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceIsEmptyString) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns"
                                           << ""),
                                NamespaceString()));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfNamespaceDoesNotMatch) {
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns"
                                           << "some string"),
                                kTestNamespace));

    // Verify that we reject the index specification when the "ns" field only contains the
    // collection name.
    ASSERT_EQ(ErrorCodes::BadValue,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.coll()),
                                kTestNamespace));
}

TEST(IndexSpecValidateTest, ReturnsIndexSpecWithNamespaceFilledInIfItIsNotPresent) {
    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1)), kTestNamespace);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns())),
                      sorted(result.getValue()));

    // Verify that the index specification we returned is still considered valid.
    ASSERT_OK(validateIndexSpec(result.getValue(), kTestNamespace));
}

TEST(IndexSpecValidateTest, ReturnsIndexSpecUnchangedIfNamespaceIsPresent) {
    auto result = validateIndexSpec(
        BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns()), kTestNamespace);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(sorted(BSON("key" << BSON("field" << 1) << "ns"
                                        << "test.index_spec_validate")),
                      sorted(result.getValue()));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsNotANumber) {
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v"
                                           << "not a number"),
                                kTestNamespace));
    ASSERT_EQ(
        ErrorCodes::TypeMismatch,
        validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << BSONObj()), kTestNamespace));
}

TEST(IndexSpecValidateTest, ReturnsAnErrorIfVersionIsV0) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 0), kTestNamespace));
}

TEST(IndexSpecValidateTest, AcceptsAnyNonZeroNumericValueForVersion) {
    auto result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 1), kTestNamespace);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 1)),
        sorted(result.getValue()));

    result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << 2.2), kTestNamespace);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << 2.2)),
        sorted(result.getValue()));

    result = validateIndexSpec(BSON("key" << BSON("field" << 1) << "v" << -3LL), kTestNamespace);
    ASSERT_OK(result.getStatus());

    // We don't care about the order of the fields in the resulting index specification.
    ASSERT_BSONOBJ_EQ(
        sorted(BSON("key" << BSON("field" << 1) << "ns" << kTestNamespace.ns() << "v" << -3LL)),
        sorted(result.getValue()));
}

}  // namespace
}  // namespace mongo
