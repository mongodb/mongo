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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo {
namespace repl {
namespace {

BSONObj makeInitReplicatedFastCountO2(StringData metadataIdent,
                                      int metadataKeyFormat,
                                      StringData timestampsIdent,
                                      int timestampsKeyFormat) {
    return BSON("fastCountMetadataStoreIdent"
                << metadataIdent << "fastCountMetadataStoreKeyFormat" << metadataKeyFormat
                << "fastCountMetadataStoreTimestampsIdent" << timestampsIdent
                << "fastCountMetadataStoreTimestampsKeyFormat" << timestampsKeyFormat);
}

TEST_F(OplogApplierImplTest, InitReplicatedFastCountCreatesRecordStores) {
    auto storageEngine = serviceContext->getStorageEngine();
    const std::string metadataIdent = storageEngine->generateNewInternalIdent();
    const std::string timestampsIdent = storageEngine->generateNewInternalIdent();

    // Verify idents do not exist before applying the oplog entry.
    {
        auto ru = storageEngine->newRecoveryUnit();
        ASSERT_FALSE(storageEngine->getEngine()->hasIdent(*ru, metadataIdent));
        ASSERT_FALSE(storageEngine->getEngine()->hasIdent(*ru, timestampsIdent));
    }

    auto op =
        makeCommandOplogEntry(nextOpTime(),
                              NamespaceString::makeCommandNamespace(DatabaseName::kAdmin),
                              BSON("initReplicatedFastCount" << 1),
                              makeInitReplicatedFastCountO2(metadataIdent,
                                                            static_cast<int>(KeyFormat::String),
                                                            timestampsIdent,
                                                            static_cast<int>(KeyFormat::Long)));

    ASSERT_OK(runOpSteadyState(op));

    // Verify both idents now exist.
    {
        auto ru = storageEngine->newRecoveryUnit();
        ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, metadataIdent));
        ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, timestampsIdent));
    }
}

TEST_F(OplogApplierImplTest, InitReplicatedFastCountWithKeyFormatLong) {
    auto storageEngine = serviceContext->getStorageEngine();
    const std::string metadataIdent = storageEngine->generateNewInternalIdent();
    const std::string timestampsIdent = storageEngine->generateNewInternalIdent();

    auto op =
        makeCommandOplogEntry(nextOpTime(),
                              NamespaceString::makeCommandNamespace(DatabaseName::kAdmin),
                              BSON("initReplicatedFastCount" << 1),
                              makeInitReplicatedFastCountO2(metadataIdent,
                                                            static_cast<int>(KeyFormat::Long),
                                                            timestampsIdent,
                                                            static_cast<int>(KeyFormat::Long)));

    ASSERT_OK(runOpSteadyState(op));

    auto ru = storageEngine->newRecoveryUnit();
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, metadataIdent));
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, timestampsIdent));
}

TEST_F(OplogApplierImplTest, InitReplicatedFastCountWithKeyFormatString) {
    auto storageEngine = serviceContext->getStorageEngine();
    const std::string metadataIdent = storageEngine->generateNewInternalIdent();
    const std::string timestampsIdent = storageEngine->generateNewInternalIdent();

    auto op =
        makeCommandOplogEntry(nextOpTime(),
                              NamespaceString::makeCommandNamespace(DatabaseName::kAdmin),
                              BSON("initReplicatedFastCount" << 1),
                              makeInitReplicatedFastCountO2(metadataIdent,
                                                            static_cast<int>(KeyFormat::String),
                                                            timestampsIdent,
                                                            static_cast<int>(KeyFormat::String)));

    ASSERT_OK(runOpSteadyState(op));

    auto ru = storageEngine->newRecoveryUnit();
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, metadataIdent));
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, timestampsIdent));
}

// TODO SERVER-122317 Test that this behavior only holds when the idents are empty.
TEST_F(OplogApplierImplTest, InitReplicatedFastCountSucceedsWhenIdentsAlreadyExist) {
    auto storageEngine = serviceContext->getStorageEngine();
    const std::string metadataIdent = storageEngine->generateNewInternalIdent();
    const std::string timestampsIdent = storageEngine->generateNewInternalIdent();

    // Pre-create the idents before applying the oplog entry.
    {
        auto& provider = rss::ReplicatedStorageService::get(_opCtx.get()).getPersistenceProvider();
        auto& ru = *shard_role_details::getRecoveryUnit(_opCtx.get());
        WriteUnitOfWork wuow(_opCtx.get());
        ASSERT_OK(storageEngine->getEngine()->createRecordStore(
            provider,
            ru,
            NamespaceString::kAdminCommandNamespace,
            metadataIdent,
            RecordStore::Options{.keyFormat = KeyFormat::String}));
        ASSERT_OK(storageEngine->getEngine()->createRecordStore(
            provider,
            ru,
            NamespaceString::kAdminCommandNamespace,
            timestampsIdent,
            RecordStore::Options{.keyFormat = KeyFormat::Long}));
        wuow.commit();
    }

    {
        auto ru = storageEngine->newRecoveryUnit();
        ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, metadataIdent));
        ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, timestampsIdent));
    }

    // Applying the oplog entry should succeed even though idents already exist.
    auto op =
        makeCommandOplogEntry(nextOpTime(),
                              NamespaceString::makeCommandNamespace(DatabaseName::kAdmin),
                              BSON("initReplicatedFastCount" << 1),
                              makeInitReplicatedFastCountO2(metadataIdent,
                                                            static_cast<int>(KeyFormat::String),
                                                            timestampsIdent,
                                                            static_cast<int>(KeyFormat::Long)));

    ASSERT_OK(runOpSteadyState(op));

    {
        auto ru = storageEngine->newRecoveryUnit();
        ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, metadataIdent));
        ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, timestampsIdent));
    }
}

TEST_F(OplogApplierImplTest, InitReplicatedFastCountRejectsInvalidMetadataKeyFormat) {
    auto storageEngine = serviceContext->getStorageEngine();
    const std::string metadataIdent = storageEngine->generateNewInternalIdent();
    const std::string timestampsIdent = storageEngine->generateNewInternalIdent();

    // keyFormat value 99 is not a valid KeyFormat enum value.
    auto op = makeCommandOplogEntry(
        nextOpTime(),
        NamespaceString::makeCommandNamespace(DatabaseName::kAdmin),
        BSON("initReplicatedFastCount" << 1),
        makeInitReplicatedFastCountO2(
            metadataIdent, 99, timestampsIdent, static_cast<int>(KeyFormat::Long)));

    ASSERT_NOT_OK(runOpSteadyState(op));

    // Neither ident should have been created.
    auto ru = storageEngine->newRecoveryUnit();
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(*ru, metadataIdent));
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(*ru, timestampsIdent));
}

TEST_F(OplogApplierImplTest, InitReplicatedFastCountRejectsInvalidTimestampsKeyFormat) {
    auto storageEngine = serviceContext->getStorageEngine();
    const std::string metadataIdent = storageEngine->generateNewInternalIdent();
    const std::string timestampsIdent = storageEngine->generateNewInternalIdent();

    // metadata keyFormat is valid, but timestamps keyFormat value -1 is invalid.
    auto op = makeCommandOplogEntry(
        nextOpTime(),
        NamespaceString::makeCommandNamespace(DatabaseName::kAdmin),
        BSON("initReplicatedFastCount" << 1),
        makeInitReplicatedFastCountO2(
            metadataIdent, static_cast<int>(KeyFormat::Long), timestampsIdent, 100));

    ASSERT_NOT_OK(runOpSteadyState(op));

    // Metadata ident should not exist because KeyFormats are validated before creation.
    auto ru = storageEngine->newRecoveryUnit();
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(*ru, metadataIdent));
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(*ru, timestampsIdent));
}

TEST_F(OplogApplierImplTest, InitReplicatedFastCountMissingO2Field) {
    auto op = makeCommandOplogEntry(nextOpTime(),
                                    NamespaceString::makeCommandNamespace(DatabaseName::kAdmin),
                                    BSON("initReplicatedFastCount" << 1));

    ASSERT_NOT_OK(runOpSteadyState(op));
}

TEST_F(OplogApplierImplTest, InitReplicatedFastCountMissingMetadataIdentField) {
    auto storageEngine = serviceContext->getStorageEngine();
    const std::string timestampsIdent = storageEngine->generateNewInternalIdent();

    auto op = makeCommandOplogEntry(
        nextOpTime(),
        NamespaceString::makeCommandNamespace(DatabaseName::kAdmin),
        BSON("initReplicatedFastCount" << 1),
        BSON("fastCountMetadataStoreKeyFormat"
             << static_cast<int>(KeyFormat::Long) << "fastCountMetadataStoreTimestampsIdent"
             << timestampsIdent << "fastCountMetadataStoreTimestampsKeyFormat"
             << static_cast<int>(KeyFormat::Long)));

    ASSERT_NOT_OK(runOpSteadyState(op));

    // Timestamp ident should not have been created.
    auto ru = storageEngine->newRecoveryUnit();
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(*ru, timestampsIdent));
}

TEST_F(OplogApplierImplTest, InitReplicatedFastCountMissingTimestampsIdentField) {
    auto storageEngine = serviceContext->getStorageEngine();
    const std::string metadataIdent = storageEngine->generateNewInternalIdent();

    auto op = makeCommandOplogEntry(nextOpTime(),
                                    NamespaceString::makeCommandNamespace(DatabaseName::kAdmin),
                                    BSON("initReplicatedFastCount" << 1),
                                    BSON("fastCountMetadataStoreIdent"
                                         << metadataIdent << "fastCountMetadataStoreKeyFormat"
                                         << static_cast<int>(KeyFormat::Long)
                                         << "fastCountMetadataStoreTimestampsKeyFormat"
                                         << static_cast<int>(KeyFormat::Long)));

    ASSERT_NOT_OK(runOpSteadyState(op));

    // Metadata ident should not have been created.
    auto ru = storageEngine->newRecoveryUnit();
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(*ru, metadataIdent));
}

TEST_F(OplogApplierImplTest, InitReplicatedFastCountRejectsDuplicateIdents) {
    auto storageEngine = serviceContext->getStorageEngine();
    const std::string sharedIdent = storageEngine->generateNewInternalIdent();

    auto op =
        makeCommandOplogEntry(nextOpTime(),
                              NamespaceString::makeCommandNamespace(DatabaseName::kAdmin),
                              BSON("initReplicatedFastCount" << 1),
                              makeInitReplicatedFastCountO2(sharedIdent,
                                                            static_cast<int>(KeyFormat::Long),
                                                            sharedIdent,
                                                            static_cast<int>(KeyFormat::Long)));

    ASSERT_NOT_OK(runOpSteadyState(op));

    // Neither ident should have been created.
    auto ru = storageEngine->newRecoveryUnit();
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(*ru, sharedIdent));
}

TEST_F(OplogApplierImplTest, InitReplicatedFastCountCreatesTimestampsWhenOnlyEmptyMetadataExists) {
    auto storageEngine = serviceContext->getStorageEngine();
    const std::string metadataIdent = storageEngine->generateNewInternalIdent();
    const std::string timestampsIdent = storageEngine->generateNewInternalIdent();

    // Pre-create only the metadata ident to simulate partial state.
    {
        auto& provider = rss::ReplicatedStorageService::get(_opCtx.get()).getPersistenceProvider();
        auto& ru = *shard_role_details::getRecoveryUnit(_opCtx.get());
        WriteUnitOfWork wuow(_opCtx.get());
        ASSERT_OK(storageEngine->getEngine()->createRecordStore(
            provider,
            ru,
            NamespaceString::kAdminCommandNamespace,
            metadataIdent,
            RecordStore::Options{.keyFormat = KeyFormat::Long}));
        wuow.commit();
    }

    {
        auto ru = storageEngine->newRecoveryUnit();
        ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, metadataIdent));
        ASSERT_FALSE(storageEngine->getEngine()->hasIdent(*ru, timestampsIdent));
    }

    auto op =
        makeCommandOplogEntry(nextOpTime(),
                              NamespaceString::makeCommandNamespace(DatabaseName::kAdmin),
                              BSON("initReplicatedFastCount" << 1),
                              makeInitReplicatedFastCountO2(metadataIdent,
                                                            static_cast<int>(KeyFormat::Long),
                                                            timestampsIdent,
                                                            static_cast<int>(KeyFormat::Long)));

    ASSERT_OK(runOpSteadyState(op));

    // Both idents should exist after we reuse the empty metadata ident and create the timestamps
    // ident.
    {
        auto ru = storageEngine->newRecoveryUnit();
        ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, metadataIdent));
        ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, timestampsIdent));
    }
}

TEST_F(OplogApplierImplTest, InitReplicatedFastCountCreatesMetadataWhenOnlyEmptyTimestampsExists) {
    auto storageEngine = serviceContext->getStorageEngine();
    const std::string metadataIdent = storageEngine->generateNewInternalIdent();
    const std::string timestampsIdent = storageEngine->generateNewInternalIdent();

    // Pre-create only the timestamps ident to simulate partial state.
    {
        auto& provider = rss::ReplicatedStorageService::get(_opCtx.get()).getPersistenceProvider();
        auto& ru = *shard_role_details::getRecoveryUnit(_opCtx.get());
        WriteUnitOfWork wuow(_opCtx.get());
        ASSERT_OK(storageEngine->getEngine()->createRecordStore(
            provider,
            ru,
            NamespaceString::kAdminCommandNamespace,
            timestampsIdent,
            RecordStore::Options{.keyFormat = KeyFormat::Long}));
        wuow.commit();
    }

    {
        auto ru = storageEngine->newRecoveryUnit();
        ASSERT_FALSE(storageEngine->getEngine()->hasIdent(*ru, metadataIdent));
        ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, timestampsIdent));
    }

    auto op =
        makeCommandOplogEntry(nextOpTime(),
                              NamespaceString::makeCommandNamespace(DatabaseName::kAdmin),
                              BSON("initReplicatedFastCount" << 1),
                              makeInitReplicatedFastCountO2(metadataIdent,
                                                            static_cast<int>(KeyFormat::Long),
                                                            timestampsIdent,
                                                            static_cast<int>(KeyFormat::Long)));

    ASSERT_OK(runOpSteadyState(op));

    // Both idents should exist after since we create the metadata ident and reuse the empty
    // timestamps ident.
    {
        auto ru = storageEngine->newRecoveryUnit();
        ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, metadataIdent));
        ASSERT_TRUE(storageEngine->getEngine()->hasIdent(*ru, timestampsIdent));
    }
}

}  // namespace
}  // namespace repl
}  // namespace mongo
