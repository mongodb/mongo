/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/validate/validate_state.h"

#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/rss/attached_storage/attached_persistence_provider.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::CollectionValidation {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.validateState");
const ValidationOptions kValidationOptions(ValidateMode::kForeground,
                                           RepairMode::kNone,
                                           /*logDiagnostics=*/false);

class ValidateStateTest : public CatalogTestFixture {};

class ReplicatedFastCountTestPersistenceProvider : public rss::AttachedPersistenceProvider {
    // Disable creating the WT size storer table.
    bool shouldUseReplicatedFastCount() const override {
        return true;
    }
};

class ValidateStateWithoutSizeStorerTest : public CatalogTestFixture {
public:
    ValidateStateWithoutSizeStorerTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<ReplicatedFastCountTestPersistenceProvider>())) {}
};

/**
 * Creates a replicated fast count collection using the global namespace string
 * kSystemReplicatedFastCountStore.
 */
void createReplicatedFastCountCollection(repl::StorageInterface* storageInterface,
                                         OperationContext* opCtx) {
    ASSERT_OK(
        storageInterface->createCollection(opCtx,
                                           NamespaceString::makeGlobalConfigCollection(
                                               NamespaceString::kSystemReplicatedFastCountStore),
                                           CollectionOptions()));
}
}  // namespace

TEST_F(ValidateStateTest, GetDetectedFastCountTypeReturnsLegazySizeStorer) {
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getDetectedFastCountType(operationContext()),
              FastCountType::legacySizeStorer);
};

TEST_F(ValidateStateTest, GetDetectedFastCountTypeReturnsBoth) {
    createReplicatedFastCountCollection(storageInterface(), operationContext());
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getDetectedFastCountType(operationContext()), FastCountType::both);
};

TEST_F(ValidateStateWithoutSizeStorerTest, GetDetectedFastCountTypeReturnsReplicated) {
    createReplicatedFastCountCollection(storageInterface(), operationContext());
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getDetectedFastCountType(operationContext()),
              FastCountType::replicated);
}

TEST_F(ValidateStateWithoutSizeStorerTest, GetDetectedFastCountTypeReturnsNeither) {
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getDetectedFastCountType(operationContext()), FastCountType::neither);
}

TEST(FastCountTypeToStringTest, Works) {
    EXPECT_EQ(toString(FastCountType::legacySizeStorer), "legacySizeStorer");
    EXPECT_EQ(toString(FastCountType::replicated), "replicated");
    EXPECT_EQ(toString(FastCountType::both), "both");
    EXPECT_EQ(toString(FastCountType::neither), "neither");
}

DEATH_TEST(FastCountToStringDeathTest, DiesOnBadFastCountType, "11853100") {
    toString(static_cast<FastCountType>(-1));
}
}  // namespace mongo::CollectionValidation
