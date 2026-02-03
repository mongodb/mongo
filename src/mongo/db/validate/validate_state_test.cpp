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
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/unittest/unittest.h"

namespace mongo::CollectionValidation {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.validateState");
const ValidationOptions kValidationOptions(ValidateMode::kForeground,
                                           RepairMode::kNone,
                                           /*logDiagnostics=*/false);

class ValidateStateTest : public CatalogTestFixture {};

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

/**
 * Deletes the internal WT size storer table. This function must be called alongside
 * cleanupDeletedWiredTigerSizeStorer() or else the CatalogTestFixture shutdown process will fail.
 */
void deleteWiredTigerSizeStorer(OperationContext* opCtx) {
    StorageEngine* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    KVEngine* kvEngine = storageEngine->getEngine();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    ASSERT_OK(kvEngine->dropIdent(ru, ident::kSizeStorer, /*identHasSizeInfo=*/true));
}

/**
 * Cleans up invalid state induced by deleteWiredTigerSizeStorer().
 *
 * See cleanup_forTest() for more information.
 */
void cleanupDeletedWiredTigerSizeStorer(OperationContext* opCtx) {
    StorageEngine* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    KVEngine* kvEngine = storageEngine->getEngine();
    auto* wtEngine = dynamic_cast<WiredTigerKVEngine*>(kvEngine);
    ASSERT(wtEngine);
    wtEngine->getSizeStorer_forTest()->cleanup_forTest();
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

TEST_F(ValidateStateTest, GetDetectedFastCountTypeReturnsReplicated) {
    createReplicatedFastCountCollection(storageInterface(), operationContext());
    deleteWiredTigerSizeStorer(operationContext());
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getDetectedFastCountType(operationContext()),
              FastCountType::replicated);
    cleanupDeletedWiredTigerSizeStorer(operationContext());
}

TEST_F(ValidateStateTest, GetDetectedFastCountTypeReturnsNeither) {
    deleteWiredTigerSizeStorer(operationContext());
    ValidateState validateState(operationContext(), kNss, kValidationOptions);
    EXPECT_EQ(validateState.getDetectedFastCountType(operationContext()), FastCountType::neither);
    cleanupDeletedWiredTigerSizeStorer(operationContext());
}
}  // namespace mongo::CollectionValidation
