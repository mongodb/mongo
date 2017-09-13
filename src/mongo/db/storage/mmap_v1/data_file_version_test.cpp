/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/storage/mmap_v1/data_file.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(DataFileVersionTest, DefaultForNewFilesIsCompatibleWithCurrentCode) {
    auto version = DataFileVersion::defaultForNewFiles();
    ASSERT_OK(version.isCompatibleWithCurrentCode());
}

TEST(DataFileVersionTest, CanSetIs24IndexClean) {
    const uint32_t major = 4;
    const uint32_t minor = 5;
    DataFileVersion version(major, minor);
    ASSERT_OK(version.isCompatibleWithCurrentCode());

    ASSERT_FALSE(version.is24IndexClean());
    version.setIs24IndexClean();
    ASSERT_TRUE(version.is24IndexClean());
}

TEST(DataFileVersionTest, CanSetMayHave30Freelist) {
    const uint32_t major = 4;
    const uint32_t minor = 5;
    DataFileVersion version(major, minor);
    ASSERT_OK(version.isCompatibleWithCurrentCode());

    ASSERT_FALSE(version.mayHave30Freelist());
    version.setMayHave30Freelist();
    ASSERT_TRUE(version.mayHave30Freelist());
}

TEST(DataFileVersionTest, CanSetMayHaveCollationMetadata) {
    auto version = DataFileVersion::defaultForNewFiles();
    ASSERT_OK(version.isCompatibleWithCurrentCode());

    ASSERT_FALSE(version.getMayHaveCollationMetadata());
    version.setMayHaveCollationMetadata();
    ASSERT_TRUE(version.getMayHaveCollationMetadata());
    ASSERT_OK(version.isCompatibleWithCurrentCode());
}

TEST(DataFileVersionTest, MustUpgradeWhenMajorVersionIsUnsupported) {
    const uint32_t major = 5;
    const uint32_t minor = 6;
    DataFileVersion version(major, minor);
    auto status = version.isCompatibleWithCurrentCode();
    ASSERT_EQ(ErrorCodes::MustUpgrade, status.code());
    ASSERT_EQ(
        "The data files have major version 5, but this version of mongod only supports version 4",
        status.reason());
}

TEST(DataFileVersionTest, MustUpgradeWhenSingleMinorFeatureBitIsUnrecognized) {
    const uint32_t major = 4;
    const uint32_t minor = 6 | (1 << 10);
    DataFileVersion version(major, minor);
    auto status = version.isCompatibleWithCurrentCode();
    ASSERT_EQ(ErrorCodes::MustUpgrade, status.code());
    ASSERT_EQ(
        "The data files use features not recognized by this version of mongod; the feature bits in"
        " positions [ 10 ] aren't recognized by this version of mongod",
        status.reason());
}

TEST(DataFileVersionTest, MustUpgradeWhenMultipleMinorFeatureBitsAreUnrecognized) {
    const uint32_t major = 4;
    const uint32_t minor = 6 | (1 << 10) | (1 << 14) | (1 << 15);
    DataFileVersion version(major, minor);
    auto status = version.isCompatibleWithCurrentCode();
    ASSERT_EQ(ErrorCodes::MustUpgrade, status.code());
    ASSERT_EQ(
        "The data files use features not recognized by this version of mongod; the feature bits in"
        " positions [ 10, 14, 15 ] aren't recognized by this version of mongod",
        status.reason());
}

TEST(DataFileVersionTest, MustUpgradeWhenIndexPluginVersionIsUnsupported) {
    const uint32_t major = 4;
    const uint32_t minor = 7;
    DataFileVersion version(major, minor);
    auto status = version.isCompatibleWithCurrentCode();
    ASSERT_EQ(ErrorCodes::MustUpgrade, status.code());
    ASSERT_EQ(
        "The data files have index plugin version 7, but this version of mongod only supports"
        " versions 5 and 6",
        status.reason());
}

}  // namespace
}  // namespace mongo
