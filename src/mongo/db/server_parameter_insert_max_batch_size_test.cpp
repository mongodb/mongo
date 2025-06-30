/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter_insert_max_batch_size.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/version/releases.h"

namespace mongo {
namespace {

TEST(InsertMaxBatchSizeTest, TestDefaultValueFcv80) {
    // Test the default value when the FCV is 8.0.
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::FeatureCompatibilityVersion::kVersion_8_0);

    ASSERT_EQ(500, getInternalInsertMaxBatchSize());
}

TEST(InsertMaxBatchSizeTest, TestDefaultValueFcv70) {
    // Test the default value when the FCV is 7.0.
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::FeatureCompatibilityVersion::kVersion_7_0);

    ASSERT_EQ(64, getInternalInsertMaxBatchSize());
}

TEST(InsertMaxBatchSizeTest, TestDefaultValueFcvUninitialized) {
    // Test the default value when the FCV is uninitialized.
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior);

    ASSERT_EQ(64, getInternalInsertMaxBatchSize());
}

TEST(InsertMaxBatchSizeTest, TestUserSetValueFcv80) {
    // Test that we always return user set value regardless of FCV.
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::FeatureCompatibilityVersion::kVersion_8_0);

    RAIIServerParameterControllerForTest batchSizeController("internalInsertMaxBatchSize", 2);

    ASSERT_EQ(2, getInternalInsertMaxBatchSize());
}

TEST(InsertMaxBatchSizeTest, TestUserSetValueFcv70) {
    // Test that we always return user set value regardless of FCV.
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::FeatureCompatibilityVersion::kVersion_7_0);

    RAIIServerParameterControllerForTest batchSizeController("internalInsertMaxBatchSize", 2);

    ASSERT_EQ(2, getInternalInsertMaxBatchSize());
}

}  // namespace
}  // namespace mongo
