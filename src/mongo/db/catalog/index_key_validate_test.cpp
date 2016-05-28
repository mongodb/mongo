/**
 *    Copyright 2015 MongoDB Inc.
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

#include <limits>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(IndexKeyValidateTest, KeyElementValueOfSmallPositiveIntSucceeds) {
    ASSERT_OK(validateKeyPattern(BSON("x" << 1)));
    ASSERT_OK(validateKeyPattern(BSON("x" << 5)));
}

TEST(IndexKeyValidateTest, KeyElementValueOfSmallNegativeIntSucceeds) {
    ASSERT_OK(validateKeyPattern(BSON("x" << -1)));
    ASSERT_OK(validateKeyPattern(BSON("x" << -5)));
}

TEST(IndexKeyValidateTest, KeyElementValueOfZeroFails) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex, validateKeyPattern(BSON("x" << 0)));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex, validateKeyPattern(BSON("x" << 0.0)));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex, validateKeyPattern(BSON("x" << -0.0)));
}

TEST(IndexKeyValidateTest, KeyElementValueOfNaNFails) {
    if (std::numeric_limits<double>::has_quiet_NaN) {
        double nan = std::numeric_limits<double>::quiet_NaN();
        ASSERT_EQ(ErrorCodes::CannotCreateIndex, validateKeyPattern(BSON("x" << nan)));
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  validateKeyPattern(BSON("a" << nan << "b"
                                              << "2d")));
    }
}

TEST(IndexKeyValidateTest, KeyElementValuePositiveFloatingPointSucceeds) {
    ASSERT_OK(validateKeyPattern(BSON("x" << 0.1)));
}

TEST(IndexKeyValidateTest, KeyElementValueNegativeFloatingPointSucceeds) {
    ASSERT_OK(validateKeyPattern(BSON("x" << -0.1)));
}

TEST(IndexKeyValidateTest, KeyElementValueOfBadPluginStringFails) {
    auto status = validateKeyPattern(BSON("x"
                                          << "foobar"));
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
}

TEST(IndexKeyValidateTest, KeyElementBooleanValueFails) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex, validateKeyPattern(BSON("x" << true)));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex, validateKeyPattern(BSON("x" << false)));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("a"
                                      << "2dsphere"
                                      << "b"
                                      << true)));
}

TEST(IndexKeyValidateTest, KeyElementNullValueFails) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex, validateKeyPattern(BSON("x" << BSONNULL)));
}

TEST(IndexKeyValidateTest, KeyElementUndefinedValueFails) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex, validateKeyPattern(BSON("x" << BSONUndefined)));
}

TEST(IndexKeyValidateTest, KeyElementMinKeyValueFails) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex, validateKeyPattern(BSON("x" << MINKEY)));
}

TEST(IndexKeyValidateTest, KeyElementMaxKeyValueFails) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex, validateKeyPattern(BSON("x" << MAXKEY)));
}

TEST(IndexKeyValidateTest, KeyElementObjectValueFails) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex, validateKeyPattern(BSON("x" << BSON("y" << 1))));
}

TEST(IndexKeyValidateTest, KeyElementArrayValueFails) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex, validateKeyPattern(BSON("x" << BSON_ARRAY(1))));
}

TEST(IndexKeyValidateTest, CompoundKeySucceedsOn2dGeoIndex) {
    ASSERT_OK(validateKeyPattern(BSON("a" << 1 << "b"
                                          << "2d")));
}

TEST(IndexKeyValidateTest, CompoundKeySucceedsOn2dsphereGeoIndex) {
    ASSERT_OK(validateKeyPattern(BSON("a" << 1 << "b"
                                          << "2dsphere")));
}

TEST(IndexKeyValidateTest, KeyElementNameTextFailsOnNonTextIndex) {
    auto status = validateKeyPattern(BSON("_fts" << 1));
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
}

TEST(IndexKeyValidateTest, KeyElementNameTextSucceedsOnTextIndex) {
    ASSERT_OK(validateKeyPattern(BSON("a" << 1 << "_fts"
                                          << "text")));
}

}  // namespace

}  // namespace mongo
