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
#include "mongo/db/commands/test_commands_enabled.h"

#include <limits>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using IndexVersion = IndexDescriptor::IndexVersion;
using index_key_validate::validateKeyPattern;

TEST(IndexKeyValidateTest, KeyElementValueOfSmallPositiveIntSucceeds) {
    for (auto indexVersion : IndexDescriptor::getSupportedIndexVersions()) {
        ASSERT_OK(validateKeyPattern(BSON("x" << 1), indexVersion));
        ASSERT_OK(validateKeyPattern(BSON("x" << 5), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementValueOfSmallNegativeIntSucceeds) {
    for (auto indexVersion : IndexDescriptor::getSupportedIndexVersions()) {
        ASSERT_OK(validateKeyPattern(BSON("x" << -1), indexVersion));
        ASSERT_OK(validateKeyPattern(BSON("x" << -5), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementValueOfZeroFailsForV2Indexes) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex, validateKeyPattern(BSON("x" << 0), IndexVersion::kV2));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("x" << 0.0), IndexVersion::kV2));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("x" << -0.0), IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementValueOfZeroSucceedsForV0AndV1Indexes) {
    for (auto indexVersion : {IndexVersion::kV0, IndexVersion::kV1}) {
        ASSERT_OK(validateKeyPattern(BSON("x" << 0), indexVersion));
        ASSERT_OK(validateKeyPattern(BSON("x" << 0.0), indexVersion));
        ASSERT_OK(validateKeyPattern(BSON("x" << -0.0), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementValueOfNaNFailsForV2Indexes) {
    if (std::numeric_limits<double>::has_quiet_NaN) {
        double nan = std::numeric_limits<double>::quiet_NaN();
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  validateKeyPattern(BSON("x" << nan), IndexVersion::kV2));
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  validateKeyPattern(BSON("a" << nan << "b"
                                              << "2d"),
                                     IndexVersion::kV2));
    }
}

TEST(IndexKeyValidateTest, KeyElementValueOfNaNSucceedsForV0AndV1Indexes) {
    if (std::numeric_limits<double>::has_quiet_NaN) {
        for (auto indexVersion : {IndexVersion::kV0, IndexVersion::kV1}) {
            double nan = std::numeric_limits<double>::quiet_NaN();
            ASSERT_OK(validateKeyPattern(BSON("x" << nan), indexVersion));
            ASSERT_OK(validateKeyPattern(BSON("a" << nan << "b"
                                                  << "2d"),
                                         indexVersion));
        }
    }
}

TEST(IndexKeyValidateTest, KeyElementValuePositiveFloatingPointSucceeds) {
    for (auto indexVersion : IndexDescriptor::getSupportedIndexVersions()) {
        ASSERT_OK(validateKeyPattern(BSON("x" << 0.1), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementValueNegativeFloatingPointSucceeds) {
    for (auto indexVersion : IndexDescriptor::getSupportedIndexVersions()) {
        ASSERT_OK(validateKeyPattern(BSON("x" << -0.1), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementValueOfBadPluginStringFails) {
    for (auto indexVersion : IndexDescriptor::getSupportedIndexVersions()) {
        auto status = validateKeyPattern(BSON("x"
                                              << "foobar"),
                                         indexVersion);
        ASSERT_NOT_OK(status);
        ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
    }
}

TEST(IndexKeyValidateTest, KeyElementBooleanValueFailsForV2Indexes) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("x" << true), IndexVersion::kV2));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("x" << false), IndexVersion::kV2));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("a"
                                      << "2dsphere"
                                      << "b"
                                      << true),
                                 IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementBooleanValueSucceedsForV0AndV1Indexes) {
    for (auto indexVersion : {IndexVersion::kV0, IndexVersion::kV1}) {
        ASSERT_OK(validateKeyPattern(BSON("x" << true), indexVersion));
        ASSERT_OK(validateKeyPattern(BSON("x" << false), indexVersion));
        ASSERT_OK(validateKeyPattern(BSON("a"
                                          << "2dsphere"
                                          << "b"
                                          << true),
                                     indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementNullValueFailsForV2Indexes) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("x" << BSONNULL), IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementNullValueSucceedsForV0AndV1Indexes) {
    for (auto indexVersion : {IndexVersion::kV0, IndexVersion::kV1}) {
        ASSERT_OK(validateKeyPattern(BSON("x" << BSONNULL), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementUndefinedValueFailsForV2Indexes) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("x" << BSONUndefined), IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementUndefinedValueSucceedsForV0AndV1Indexes) {
    for (auto indexVersion : {IndexVersion::kV0, IndexVersion::kV1}) {
        ASSERT_OK(validateKeyPattern(BSON("x" << BSONUndefined), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementMinKeyValueFailsForV2Indexes) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("x" << MINKEY), IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementMinKeyValueSucceedsForV0AndV1Indexes) {
    for (auto indexVersion : {IndexVersion::kV0, IndexVersion::kV1}) {
        ASSERT_OK(validateKeyPattern(BSON("x" << MINKEY), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementMaxKeyValueFailsForV2Indexes) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("x" << MAXKEY), IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementMaxKeyValueSucceedsForV0AndV1Indexes) {
    for (auto indexVersion : {IndexVersion::kV0, IndexVersion::kV1}) {
        ASSERT_OK(validateKeyPattern(BSON("x" << MAXKEY), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementObjectValueFails) {
    for (auto indexVersion : IndexDescriptor::getSupportedIndexVersions()) {
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  validateKeyPattern(BSON("x" << BSON("y" << 1)), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementArrayValueFails) {
    for (auto indexVersion : IndexDescriptor::getSupportedIndexVersions()) {
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  validateKeyPattern(BSON("x" << BSON_ARRAY(1)), indexVersion));
    }
}

TEST(IndexKeyValidateTest, CompoundKeySucceedsOn2dGeoIndex) {
    for (auto indexVersion : IndexDescriptor::getSupportedIndexVersions()) {
        ASSERT_OK(validateKeyPattern(BSON("a" << 1 << "b"
                                              << "2d"),
                                     indexVersion));
    }
}

TEST(IndexKeyValidateTest, CompoundKeySucceedsOn2dsphereGeoIndex) {
    for (auto indexVersion : IndexDescriptor::getSupportedIndexVersions()) {
        ASSERT_OK(validateKeyPattern(BSON("a" << 1 << "b"
                                              << "2dsphere"),
                                     indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementNameTextFailsOnNonTextIndex) {
    for (auto indexVersion : IndexDescriptor::getSupportedIndexVersions()) {
        auto status = validateKeyPattern(BSON("_fts" << 1), indexVersion);
        ASSERT_NOT_OK(status);
        ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
    }
}

TEST(IndexKeyValidateTest, KeyElementNameTextSucceedsOnTextIndex) {
    for (auto indexVersion : IndexDescriptor::getSupportedIndexVersions()) {
        ASSERT_OK(validateKeyPattern(BSON("a" << 1 << "_fts"
                                              << "text"),
                                     indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementNameAllPathsSucceedsOnSubPath) {
    const bool temp = getTestCommandsEnabled();
    setTestCommandsEnabled(true);
    ASSERT_OK(validateKeyPattern(BSON("a.$**" << 1), IndexVersion::kV2));
    setTestCommandsEnabled(temp);
}

TEST(IndexKeyValidateTest, KeyElementNameAllPathsSucceeds) {
    const bool temp = getTestCommandsEnabled();
    setTestCommandsEnabled(true);
    ASSERT_OK(validateKeyPattern(BSON("$**" << 1), IndexVersion::kV2));
    setTestCommandsEnabled(temp);
}

TEST(IndexKeyValidateTest, KeyElementNameAllPathsFailsOnRepeat) {
    const bool temp = getTestCommandsEnabled();
    setTestCommandsEnabled(true);
    auto status = validateKeyPattern(BSON("$**.$**" << 1), IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
    setTestCommandsEnabled(temp);
}

TEST(IndexKeyValidateTest, KeyElementNameAllPathsFailsOnSubPathRepeat) {
    const bool temp = getTestCommandsEnabled();
    setTestCommandsEnabled(true);
    auto status = validateKeyPattern(BSON("a.$**.$**" << 1), IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
    setTestCommandsEnabled(temp);
}

TEST(IndexKeyValidateTest, KeyElementNameAllPathsFailsOnCompound) {
    const bool temp = getTestCommandsEnabled();
    setTestCommandsEnabled(true);
    auto status = validateKeyPattern(BSON("$**" << 1 << "a" << 1), IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
    setTestCommandsEnabled(temp);
}

TEST(IndexKeyValidateTest, KeyElementNameAllPathsFailsOnIncorrectValue) {
    const bool temp = getTestCommandsEnabled();
    setTestCommandsEnabled(true);
    auto status = validateKeyPattern(BSON("$**" << false), IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
    setTestCommandsEnabled(temp);
}

}  // namespace

}  // namespace mongo
