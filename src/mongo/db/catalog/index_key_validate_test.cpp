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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_key_validate.h"

#include <limits>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using IndexVersion = IndexDescriptor::IndexVersion;
using index_key_validate::validateKeyPattern;

/**
 * Returns a set of the currently supported index versions.
 */
std::set<IndexVersion> getSupportedIndexVersions() {
    return {IndexVersion::kV1, IndexVersion::kV2};
}


TEST(IndexKeyValidateTest, KeyElementValueOfSmallPositiveIntSucceeds) {
    for (auto indexVersion : getSupportedIndexVersions()) {
        ASSERT_OK(validateKeyPattern(BSON("x" << 1), indexVersion));
        ASSERT_OK(validateKeyPattern(BSON("x" << 5), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementValueOfSmallNegativeIntSucceeds) {
    for (auto indexVersion : getSupportedIndexVersions()) {
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

TEST(IndexKeyValidateTest, KeyElementValueOfZeroSucceedsForV1Indexes) {
    ASSERT_OK(validateKeyPattern(BSON("x" << 0), IndexVersion::kV1));
    ASSERT_OK(validateKeyPattern(BSON("x" << 0.0), IndexVersion::kV1));
    ASSERT_OK(validateKeyPattern(BSON("x" << -0.0), IndexVersion::kV1));
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

TEST(IndexKeyValidateTest, KeyElementValueOfNaNSucceedsForV1Indexes) {
    if (std::numeric_limits<double>::has_quiet_NaN) {
        double nan = std::numeric_limits<double>::quiet_NaN();
        ASSERT_OK(validateKeyPattern(BSON("x" << nan), IndexVersion::kV1));
        ASSERT_OK(validateKeyPattern(BSON("a" << nan << "b"
                                              << "2d"),
                                     IndexVersion::kV1));
    }
}

TEST(IndexKeyValidateTest, KeyElementValuePositiveFloatingPointSucceeds) {
    for (auto indexVersion : getSupportedIndexVersions()) {
        ASSERT_OK(validateKeyPattern(BSON("x" << 0.1), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementValueNegativeFloatingPointSucceeds) {
    for (auto indexVersion : getSupportedIndexVersions()) {
        ASSERT_OK(validateKeyPattern(BSON("x" << -0.1), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementValueOfBadPluginStringFails) {
    for (auto indexVersion : getSupportedIndexVersions()) {
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
                                      << "b" << true),
                                 IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementBooleanValueSucceedsForV1Indexes) {
    ASSERT_OK(validateKeyPattern(BSON("x" << true), IndexVersion::kV1));
    ASSERT_OK(validateKeyPattern(BSON("x" << false), IndexVersion::kV1));
    ASSERT_OK(validateKeyPattern(BSON("a"
                                      << "2dsphere"
                                      << "b" << true),
                                 IndexVersion::kV1));
}

TEST(IndexKeyValidateTest, KeyElementNullValueFailsForV2Indexes) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("x" << BSONNULL), IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementNullValueSucceedsForV1Indexes) {
    ASSERT_OK(validateKeyPattern(BSON("x" << BSONNULL), IndexVersion::kV1));
}

TEST(IndexKeyValidateTest, KeyElementUndefinedValueFailsForV2Indexes) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("x" << BSONUndefined), IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementUndefinedValueSucceedsForV1Indexes) {
    ASSERT_OK(validateKeyPattern(BSON("x" << BSONUndefined), IndexVersion::kV1));
}

TEST(IndexKeyValidateTest, KeyElementMinKeyValueFailsForV2Indexes) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("x" << MINKEY), IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementMinKeyValueSucceedsForV1Indexes) {
    ASSERT_OK(validateKeyPattern(BSON("x" << MINKEY), IndexVersion::kV1));
}

TEST(IndexKeyValidateTest, KeyElementMaxKeyValueFailsForV2Indexes) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("x" << MAXKEY), IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementMaxKeyValueSucceedsForV1Indexes) {
    ASSERT_OK(validateKeyPattern(BSON("x" << MAXKEY), IndexVersion::kV1));
}

TEST(IndexKeyValidateTest, KeyElementObjectValueFails) {
    for (auto indexVersion : getSupportedIndexVersions()) {
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  validateKeyPattern(BSON("x" << BSON("y" << 1)), indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementArrayValueFails) {
    for (auto indexVersion : getSupportedIndexVersions()) {
        ASSERT_EQ(ErrorCodes::CannotCreateIndex,
                  validateKeyPattern(BSON("x" << BSON_ARRAY(1)), indexVersion));
    }
}

TEST(IndexKeyValidateTest, CompoundKeySucceedsOn2dGeoIndex) {
    for (auto indexVersion : getSupportedIndexVersions()) {
        ASSERT_OK(validateKeyPattern(BSON("a" << 1 << "b"
                                              << "2d"),
                                     indexVersion));
    }
}

TEST(IndexKeyValidateTest, CompoundKeySucceedsOn2dsphereGeoIndex) {
    for (auto indexVersion : getSupportedIndexVersions()) {
        ASSERT_OK(validateKeyPattern(BSON("a" << 1 << "b"
                                              << "2dsphere"),
                                     indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementNameTextFailsOnNonTextIndex) {
    for (auto indexVersion : getSupportedIndexVersions()) {
        auto status = validateKeyPattern(BSON("_fts" << 1), indexVersion);
        ASSERT_NOT_OK(status);
        ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
    }
}

TEST(IndexKeyValidateTest, KeyElementNameTextSucceedsOnTextIndex) {
    for (auto indexVersion : getSupportedIndexVersions()) {
        ASSERT_OK(validateKeyPattern(BSON("a" << 1 << "_fts"
                                              << "text"),
                                     indexVersion));
    }
}

TEST(IndexKeyValidateTest, KeyElementNameWildcardSucceedsOnSubPath) {
    ASSERT_OK(validateKeyPattern(BSON("a.$**" << 1), IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementNameWildcardSucceeds) {
    ASSERT_OK(validateKeyPattern(BSON("$**" << 1), IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, WildcardIndexNumericKeyElementValueFailsIfNotPositive) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("$**" << 0.0), IndexVersion::kV2));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("$**" << -0.0), IndexVersion::kV2));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("$**" << -0.1), IndexVersion::kV2));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("$**" << -1), IndexVersion::kV2));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("$**" << INT_MIN), IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementNameWildcardFailsOnRepeat) {
    auto status = validateKeyPattern(BSON("$**.$**" << 1), IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
}

TEST(IndexKeyValidateTest, KeyElementNameWildcardFailsOnSubPathRepeat) {
    auto status = validateKeyPattern(BSON("a.$**.$**" << 1), IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
}

TEST(IndexKeyValidateTest, KeyElementNameWildcardFailsOnCompound) {
    auto status = validateKeyPattern(BSON("$**" << 1 << "a" << 1), IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
}

TEST(IndexKeyValidateTest, KeyElementNameWildcardFailsOnIncorrectValue) {
    auto status = validateKeyPattern(BSON("$**" << false), IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
}

TEST(IndexKeyValidateTest, KeyElementNameWildcardFailsWhenValueIsPluginNameWithInvalidKeyName) {
    auto status = validateKeyPattern(BSON("a"
                                          << "wildcard"),
                                     IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
}

TEST(IndexKeyValidateTest, KeyElementNameWildcardFailsWhenValueIsPluginNameWithValidKeyName) {
    auto status = validateKeyPattern(BSON("$**"
                                          << "wildcard"),
                                     IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
}

TEST(IndexKeyValidateTest, KeyElementNameColumnstoreSucceedsOnSubPath) {
    ASSERT_OK(validateKeyPattern(BSON("a.$**"
                                      << "columnstore"),
                                 IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementNameColumnstoreSucceeds) {
    ASSERT_OK(validateKeyPattern(BSON("$**"
                                      << "columnstore"),
                                 IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, ColumnstoreIndexKeyElementFailsForInvalidValue) {
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("$**"
                                      << "columnStore"),
                                 IndexVersion::kV2));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("$**"
                                      << "notColumnstore"),
                                 IndexVersion::kV2));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("$**"
                                      << "column store"),
                                 IndexVersion::kV2));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("$**"
                                      << "cs"),
                                 IndexVersion::kV2));
    ASSERT_EQ(ErrorCodes::CannotCreateIndex,
              validateKeyPattern(BSON("$**"
                                      << "string"),
                                 IndexVersion::kV2));
}

TEST(IndexKeyValidateTest, KeyElementNameColumnstoreFailsOnRepeat) {
    auto status = validateKeyPattern(BSON("$**.$**"
                                          << "columnstore"),
                                     IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
}

TEST(IndexKeyValidateTest, KeyElementNameColumnstoreFailsOnSubPathRepeat) {
    auto status = validateKeyPattern(BSON("a.$**.$**"
                                          << "columnstore"),
                                     IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
}

TEST(IndexKeyValidateTest, KeyElementNameColumnstoreFailsOnCompound) {
    auto status = validateKeyPattern(BSON("$**"
                                          << "columnstore"
                                          << "a"
                                          << "columnstore"),
                                     IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
}

TEST(IndexKeyValidateTest, KeyElementNameColumnstoreFailsOnIncorrectValue) {
    auto status = validateKeyPattern(BSON("$**" << true), IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
}

TEST(IndexKeyValidateTest, KeyElementNameColumnstoreFailsWhenValueIsPluginNameWithInvalidKeyName) {
    auto status = validateKeyPattern(BSON("a"
                                          << "columnstore"),
                                     IndexVersion::kV2);
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status, ErrorCodes::CannotCreateIndex);
}

TEST(IndexKeyValidateTest, CompoundHashedIndex) {
    // Validation succeeds with hashed prefix in the index.
    ASSERT_OK(index_key_validate::validateIndexSpec(
        nullptr, fromjson("{key: {a : 'hashed', b: 1}, name: 'index'}")));

    // Validation succeeds with non-hashed prefix in the index.
    ASSERT_OK(index_key_validate::validateIndexSpec(
        nullptr, fromjson("{key: {b: 1, a : 'hashed', c: 1}, name: 'index'}")));
}

TEST(IndexKeyValidateTest, Background) {
    ASSERT_OK(index_key_validate::validateIndexSpec(
        nullptr, fromjson("{key: {a: 1}, name: 'index', background: true}")));

    ASSERT_OK(index_key_validate::validateIndexSpec(
        nullptr, fromjson("{key: {a: 1}, name: 'index', background: 1}")));

    ASSERT_OK(index_key_validate::validateIndexSpec(
        nullptr, fromjson("{key: {a: 1}, name: 'index', background: 0}")));

    ASSERT_NOT_OK(index_key_validate::validateIndexSpec(
        nullptr, fromjson("{key: {a: 1}, name: 'index', background: 'foo'}")));

    ASSERT_NOT_OK(index_key_validate::validateIndexSpec(
        nullptr, fromjson("{key: {a: 1}, name: 'index', background: []}")));
}

TEST(IndexKeyValidateTest, RemoveUnkownFieldsFromIndexSpecs) {
    ASSERT(fromjson("{key: {a: 1}, name: 'index'}")
               .binaryEqual(index_key_validate::removeUnknownFields(
                   NamespaceString("coll"),
                   fromjson("{key: {a: 1}, name: 'index', safe: true, force: true}"))));
}

TEST(IndexKeyValidateTest, RepairIndexSpecs) {
    ASSERT(fromjson("{key: {a: 1}, name: 'index'}")
               .binaryEqual(index_key_validate::repairIndexSpec(
                   NamespaceString("coll"),
                   fromjson("{key: {a: 1}, name: 'index', safe: true, force: true}"))));

    ASSERT(fromjson("{key: {a: 1}, name: 'index', sparse: true}")
               .binaryEqual(index_key_validate::repairIndexSpec(
                   NamespaceString("coll"),
                   fromjson("{key: {a: 1}, name: 'index', sparse: 'true'}"))));

    ASSERT(fromjson("{key: {a: 1}, name: 'index', background: true}")
               .binaryEqual(index_key_validate::repairIndexSpec(
                   NamespaceString("coll"),
                   fromjson("{key: {a: 1}, name: 'index', background: '1'}"))));

    ASSERT(fromjson("{key: {a: 1}, name: 'index', sparse: true, background: true}")
               .binaryEqual(index_key_validate::repairIndexSpec(
                   NamespaceString("coll"),
                   fromjson("{key: {a: 1}, name: 'index', sparse: 'true', background: '1'}"))));

    ASSERT(fromjson("{key: {a: 1}, name: 'index', sparse: true, background: true}")
               .binaryEqual(index_key_validate::repairIndexSpec(
                   NamespaceString("coll"),
                   fromjson("{key: {a: 1}, name: 'index', sparse: 'true', background: '1', safe: "
                            "true, force: true}"))));

    ASSERT(BSON("key" << BSON("a" << 1) << "name"
                      << "index"
                      << "expireAfterSeconds" << std::numeric_limits<int32_t>::max())
               .binaryEqual(index_key_validate::repairIndexSpec(
                   NamespaceString("coll"),
                   fromjson("{key: {a: 1}, name: 'index', expireAfterSeconds: NaN}"))));

    ASSERT(BSON("key" << BSON("a" << 1) << "name"
                      << "index"
                      << "expireAfterSeconds" << std::numeric_limits<int32_t>::max())
               .binaryEqual(index_key_validate::repairIndexSpec(
                   NamespaceString("coll"),
                   fromjson("{key: {a: 1}, name: 'index', expireAfterSeconds: '123'}"))));
}

TEST(IndexKeyValidateTest, GeoIndexSpecs) {
    ASSERT_OK(index_key_validate::validateIndexSpec(
        nullptr,
        fromjson("{'key':{'loc':'2dsphere'},'name':'loc_2dsphere','finestIndexedLevel':17,'"
                 "coarsestIndexedLevel':5}")));

    ASSERT_NOT_OK(index_key_validate::validateIndexSpec(
        nullptr,
        fromjson("{'key':{'loc':'2dsphere'},'name':'loc_2dsphere','finestIndexedLevel':'string','"
                 "coarsestIndexedLevel':'string'}")));

    ASSERT_NOT_OK(index_key_validate::validateIndexSpec(
        nullptr,
        fromjson("{'key':{'loc':'2dsphere'},'name':'loc_2dsphere','finestIndexedLevel':17,'"
                 "coarsestIndexedLevel':'string'}")));

    ASSERT_NOT_OK(index_key_validate::validateIndexSpec(
        nullptr,
        fromjson("{'key':{'loc':'2dsphere'},'name':'loc_2dsphere','finestIndexedLevel':'string','"
                 "coarsestIndexedLevel':5}")));

    ASSERT_NOT_OK(index_key_validate::validateIndexSpec(
        nullptr,
        fromjson("{'key':{'loc':'2dsphere'},'name':'loc_2dsphere','finestIndexedLevel':true,'"
                 "coarsestIndexedLevel':true}")));

    ASSERT_NOT_OK(index_key_validate::validateIndexSpec(
        nullptr,
        fromjson("{'key':{'loc':'2dsphere'},'name':'loc_2dsphere','finestIndexedLevel':17,'"
                 "coarsestIndexedLevel':true}")));

    ASSERT_NOT_OK(index_key_validate::validateIndexSpec(
        nullptr,
        fromjson("{'key':{'loc':'2dsphere'},'name':'loc_2dsphere','finestIndexedLevel':true,'"
                 "coarsestIndexedLevel':5}")));
}

}  // namespace
}  // namespace mongo
