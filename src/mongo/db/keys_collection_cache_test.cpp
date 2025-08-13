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

#include "mongo/db/keys_collection_cache.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/keys_collection_client_direct.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/update_result.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class CacheTest : public ConfigServerTestFixture {
protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();

        _catalogClient = std::make_unique<KeysCollectionClientSharded>(
            Grid::get(operationContext())->catalogClient());

        _directClient = std::make_unique<KeysCollectionClientDirect>(false /*mustUseLocalReads*/);
    }

    KeysCollectionClient* catalogClient() const {
        return _catalogClient.get();
    }

    KeysCollectionClient* directClient() const {
        return _directClient.get();
    }

    void insertDocument(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& doc) {
        auto collection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        auto updateResult = Helpers::upsert(opCtx, collection, doc);
        ASSERT_EQ(0, updateResult.numDocsModified);
    }

    void deleteDocument(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj& filter) {
        auto cmdObj = [&] {
            write_ops::DeleteCommandRequest deleteOp(nss);
            deleteOp.setDeletes({[&] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(filter);
                entry.setMulti(false);
                return entry;
            }()});
            return deleteOp.toBSON();
        }();

        DBDirectClient client(opCtx);
        BSONObj result;
        client.runCommand(nss.dbName(), cmdObj, result);
        ASSERT_OK(getStatusFromWriteCommandReply(result));
    }

    void updateDocument(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj& filter,
                        const BSONObj& update) {
        auto cmdObj = [&] {
            write_ops::UpdateCommandRequest updateOp(nss);
            updateOp.setUpdates({[&] {
                write_ops::UpdateOpEntry entry;
                entry.setQ(filter);
                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
                return entry;
            }()});
            return updateOp.toBSON();
        }();

        DBDirectClient client(opCtx);
        BSONObj result;
        client.runCommand(nss.dbName(), cmdObj, result);
        ASSERT_OK(getStatusFromWriteCommandReply(result));
    }

    void testRefreshDoesNotErrorIfExternalKeysCacheIsEmpty(KeysCollectionClient* client);

    void testGetKeyShouldReturnCorrectKeysAfterRefresh(KeysCollectionClient* client);

    void testGetInternalKeyShouldReturnErrorIfNoKeyIsValidForGivenTime(
        KeysCollectionClient* client);

    void testGetInternalKeyShouldReturnOldestKeyPossible(KeysCollectionClient* client);

    void testRefreshShouldNotGetInternalKeysForOtherPurpose(KeysCollectionClient* client);

    void testRefreshShouldNotGetExternalKeysForOtherPurpose(KeysCollectionClient* client);

    void testGetRefreshCanIncrementallyGetNewKeys(KeysCollectionClient* client);

    void testCacheExternalKeyBasic(KeysCollectionClient* client);

    void testRefreshClearsRemovedExternalKeys(KeysCollectionClient* client);

    void testRefreshHandlesKeysReceivingTTLValue(KeysCollectionClient* client);

private:
    std::unique_ptr<KeysCollectionClient> _catalogClient;
    std::unique_ptr<KeysCollectionClient> _directClient;
};

TEST_F(CacheTest, GetInternalKeyErrorsIfInternalKeysCacheIsEmpty) {
    KeysCollectionCache cache("test", catalogClient());
    auto status = cache.getInternalKey(LogicalTime(Timestamp(1, 0))).getStatus();
    ASSERT_EQ(ErrorCodes::KeyNotFound, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(CacheTest, GetInternalKeyByIdErrorsIfInternalKeysCacheIsEmpty) {
    KeysCollectionCache cache("test", catalogClient());
    auto status = cache.getInternalKeyById(1, LogicalTime(Timestamp(1, 0))).getStatus();
    ASSERT_EQ(ErrorCodes::KeyNotFound, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(CacheTest, GetExternalKeysByIdErrorsIfExternalKeysCacheIsEmpty) {
    KeysCollectionCache cache("test", catalogClient());
    auto status = cache.getExternalKeysById(1, LogicalTime(Timestamp(1, 0))).getStatus();
    ASSERT_EQ(ErrorCodes::KeyNotFound, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(CacheTest, RefreshErrorsIfInternalCacheIsEmpty) {
    KeysCollectionCache cache("test", catalogClient());
    auto status = cache.refresh(operationContext()).getStatus();
    ASSERT_EQ(ErrorCodes::KeyNotFound, status.code());
    ASSERT_FALSE(status.reason().empty());
}

void CacheTest::testRefreshDoesNotErrorIfExternalKeysCacheIsEmpty(KeysCollectionClient* client) {
    KeysCollectionCache cache("test", client);

    KeysCollectionDocument origKey1(1);
    origKey1.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey1.toBSON()));

    auto status = cache.refresh(operationContext()).getStatus();
    ASSERT_OK(status);
}

TEST_F(CacheTest, RefreshDoesNotErrorIfExternalKeysCacheIsEmptyShardedClient) {
    testRefreshDoesNotErrorIfExternalKeysCacheIsEmpty(catalogClient());
}

TEST_F(CacheTest, RefreshDoesNotErrorIfExternalKeysCacheIsEmptyDirectClient) {
    testRefreshDoesNotErrorIfExternalKeysCacheIsEmpty(directClient());
}

void CacheTest::testGetKeyShouldReturnCorrectKeysAfterRefresh(KeysCollectionClient* client) {
    KeysCollectionCache cache("test", client);
    KeysCollectionDocument origKey0(1);
    origKey0.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    insertDocument(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey0.toBSON());

    // Use external keys with the same keyId and expiresAt as the internal key to test that the
    // cache correctly tackles key collisions.
    ExternalKeysCollectionDocument origKey1(OID::gen(), 1);
    origKey1.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    origKey1.setTTLExpiresAt(getServiceContext()->getFastClockSource()->now() + Seconds(30));
    insertDocument(
        operationContext(), NamespaceString::kExternalKeysCollectionNamespace, origKey1.toBSON());

    ExternalKeysCollectionDocument origKey2(OID::gen(), 1);
    origKey2.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(205, 0))});
    insertDocument(
        operationContext(), NamespaceString::kExternalKeysCollectionNamespace, origKey2.toBSON());

    auto refreshStatus = cache.refresh(operationContext());
    ASSERT_OK(refreshStatus.getStatus());

    // refresh() should return the internal key.
    {
        auto key = refreshStatus.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey0.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }

    auto swInternalKey = cache.getInternalKey(LogicalTime(Timestamp(1, 0)));
    ASSERT_OK(swInternalKey.getStatus());

    {
        auto key = swInternalKey.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey0.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }

    swInternalKey = cache.getInternalKeyById(1, LogicalTime(Timestamp(1, 0)));
    ASSERT_OK(swInternalKey.getStatus());

    {
        auto key = swInternalKey.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey0.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }

    auto swExternalKeys = cache.getExternalKeysById(1, LogicalTime(Timestamp(1, 0)));
    ASSERT_OK(swExternalKeys.getStatus());

    {
        std::map<OID, ExternalKeysCollectionDocument> expectedKeys = {
            {origKey1.getId(), origKey1},
            {origKey2.getId(), origKey2},
        };

        auto keys = swExternalKeys.getValue();
        ASSERT_EQ(expectedKeys.size(), keys.size());

        for (const auto& key : keys) {
            auto iter = expectedKeys.find(key.getId());
            ASSERT(iter != expectedKeys.end());

            auto expectedKey = iter->second;
            ASSERT_EQ(expectedKey.getId(), key.getId());
            ASSERT_EQ(expectedKey.getPurpose(), key.getPurpose());
            ASSERT_EQ(expectedKey.getExpiresAt().asTimestamp(), key.getExpiresAt().asTimestamp());
            if (expectedKey.getTTLExpiresAt()) {
                ASSERT_EQ(*expectedKey.getTTLExpiresAt(), *key.getTTLExpiresAt());
            } else {
                ASSERT(!expectedKey.getTTLExpiresAt()), key.getTTLExpiresAt();
                ASSERT(!key.getTTLExpiresAt());
            }
        }
    }

    swExternalKeys = cache.getExternalKeysById(1, LogicalTime(Timestamp(150, 0)));
    ASSERT_OK(swExternalKeys.getStatus());

    {
        auto keys = swExternalKeys.getValue();
        ASSERT_EQ(1, keys.size());
        auto key = keys.front();

        ASSERT_EQ(origKey2.getId(), key.getId());
        ASSERT_EQ(origKey2.getPurpose(), key.getPurpose());
        ASSERT_EQ(origKey2.getExpiresAt().asTimestamp(), key.getExpiresAt().asTimestamp());
        ASSERT(!key.getTTLExpiresAt());
    }

    swExternalKeys = cache.getExternalKeysById(1, LogicalTime(Timestamp(300, 0)));
    ASSERT_EQ(ErrorCodes::KeyNotFound, swExternalKeys.getStatus());
}

TEST_F(CacheTest, GetKeyShouldReturnCorrectKeyAfterRefreshShardedClient) {
    testGetKeyShouldReturnCorrectKeysAfterRefresh(catalogClient());
}

TEST_F(CacheTest, GetKeyShouldReturnCorrectKeysAfterRefreshDirectClient) {
    testGetKeyShouldReturnCorrectKeysAfterRefresh(directClient());
}

void CacheTest::testGetInternalKeyShouldReturnErrorIfNoKeyIsValidForGivenTime(
    KeysCollectionClient* client) {
    KeysCollectionCache cache("test", client);

    KeysCollectionDocument origKey1(1);
    origKey1.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey1.toBSON()));

    auto refreshStatus = cache.refresh(operationContext());
    ASSERT_OK(refreshStatus.getStatus());

    {
        auto key = refreshStatus.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }

    auto swKey = cache.getInternalKey(LogicalTime(Timestamp(110, 0)));
    ASSERT_EQ(ErrorCodes::KeyNotFound, swKey.getStatus());
}

TEST_F(CacheTest, GetInternalKeyShouldReturnErrorIfNoKeyIsValidForGivenTimeShardedClient) {
    testGetInternalKeyShouldReturnErrorIfNoKeyIsValidForGivenTime(catalogClient());
}

TEST_F(CacheTest, GetInternalKeyShouldReturnErrorIfNoKeyIsValidForGivenTimeDirectClient) {
    testGetInternalKeyShouldReturnErrorIfNoKeyIsValidForGivenTime(directClient());
}

void CacheTest::testGetInternalKeyShouldReturnOldestKeyPossible(KeysCollectionClient* client) {
    KeysCollectionCache cache("test", client);

    KeysCollectionDocument origKey0(0);
    origKey0.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(100, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey0.toBSON()));

    KeysCollectionDocument origKey1(1);
    origKey1.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey1.toBSON()));

    KeysCollectionDocument origKey2(2);
    origKey2.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(110, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey2.toBSON()));

    auto refreshStatus = cache.refresh(operationContext());
    ASSERT_OK(refreshStatus.getStatus());

    {
        auto key = refreshStatus.getValue();
        ASSERT_EQ(2, key.getKeyId());
        ASSERT_EQ(origKey2.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(110, 0), key.getExpiresAt().asTimestamp());
    }

    auto swKey = cache.getInternalKey(LogicalTime(Timestamp(103, 1)));
    ASSERT_OK(swKey.getStatus());

    {
        auto key = swKey.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }
}

TEST_F(CacheTest, GetInternalKeyShouldReturnOldestKeyPossibleShardedClient) {
    testGetInternalKeyShouldReturnOldestKeyPossible(catalogClient());
}

TEST_F(CacheTest, GetInternalKeyShouldReturnOldestKeyPossibleDirectClient) {
    testGetInternalKeyShouldReturnOldestKeyPossible(directClient());
}

void CacheTest::testRefreshShouldNotGetInternalKeysForOtherPurpose(KeysCollectionClient* client) {
    KeysCollectionCache cache("test", client);

    KeysCollectionDocument origKey0(0);
    origKey0.setKeysCollectionDocumentBase(
        {"dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(100, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey0.toBSON()));

    {
        auto refreshStatus = cache.refresh(operationContext());
        ASSERT_EQ(ErrorCodes::KeyNotFound, refreshStatus.getStatus());

        auto swKey = cache.getInternalKey(LogicalTime(Timestamp(50, 0)));
        ASSERT_EQ(ErrorCodes::KeyNotFound, swKey.getStatus());
    }

    KeysCollectionDocument origKey1(1);
    origKey1.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey1.toBSON()));

    {
        auto refreshStatus = cache.refresh(operationContext());
        ASSERT_OK(refreshStatus.getStatus());

        auto key = refreshStatus.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }

    auto swKey = cache.getInternalKey(LogicalTime(Timestamp(60, 1)));
    ASSERT_OK(swKey.getStatus());

    {
        auto key = swKey.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }
}

TEST_F(CacheTest, RefreshShouldNotGetInternalKeysForOtherPurposeShardedClient) {
    testRefreshShouldNotGetInternalKeysForOtherPurpose(catalogClient());
}

TEST_F(CacheTest, RefreshShouldNotGetInternalKeysForOtherPurposeDirectClient) {
    testRefreshShouldNotGetInternalKeysForOtherPurpose(directClient());
}

void CacheTest::testRefreshShouldNotGetExternalKeysForOtherPurpose(KeysCollectionClient* client) {
    KeysCollectionCache cache("test", client);

    KeysCollectionDocument origKey0(0);
    origKey0.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(100, 0))});
    insertDocument(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey0.toBSON());

    ExternalKeysCollectionDocument origKey1(OID::gen(), 1);
    origKey1.setKeysCollectionDocumentBase(
        {"dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    insertDocument(
        operationContext(), NamespaceString::kExternalKeysCollectionNamespace, origKey1.toBSON());

    {
        auto refreshStatus = cache.refresh(operationContext());
        ASSERT_OK(refreshStatus.getStatus());

        auto swKey = cache.getExternalKeysById(1, LogicalTime(Timestamp(1, 0)));
        ASSERT_EQ(ErrorCodes::KeyNotFound, swKey.getStatus());
    }

    ExternalKeysCollectionDocument origKey2(OID::gen(), 2);
    origKey2.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(110, 0))});
    insertDocument(
        operationContext(), NamespaceString::kExternalKeysCollectionNamespace, origKey2.toBSON());

    {
        auto refreshStatus = cache.refresh(operationContext());
        ASSERT_OK(refreshStatus.getStatus());

        auto swKeys = cache.getExternalKeysById(2, LogicalTime(Timestamp(1, 0)));
        ASSERT_OK(swKeys.getStatus());

        auto keys = swKeys.getValue();
        ASSERT_EQ(1, keys.size());

        auto key = keys.front();
        ASSERT_EQ(2, key.getKeyId());
        ASSERT_EQ(origKey2.getId(), key.getId());
        ASSERT_EQ(origKey2.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(110, 0), key.getExpiresAt().asTimestamp());
    }
}

TEST_F(CacheTest, RefreshShouldNotGetExternalKeysForOtherPurposeShardedClient) {
    testRefreshShouldNotGetExternalKeysForOtherPurpose(catalogClient());
}

TEST_F(CacheTest, RefreshShouldNotGetExternalKeysForOtherPurposeDirectClient) {
    testRefreshShouldNotGetExternalKeysForOtherPurpose(directClient());
}

void CacheTest::testGetRefreshCanIncrementallyGetNewKeys(KeysCollectionClient* client) {
    KeysCollectionCache cache("test", client);

    KeysCollectionDocument origKey0(0);
    origKey0.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(100, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey0.toBSON()));

    {
        auto refreshStatus = cache.refresh(operationContext());
        ASSERT_OK(refreshStatus.getStatus());


        auto key = refreshStatus.getValue();
        ASSERT_EQ(0, key.getKeyId());
        ASSERT_EQ(origKey0.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(100, 0), key.getExpiresAt().asTimestamp());

        auto swKey = cache.getInternalKey(LogicalTime(Timestamp(112, 1)));
        ASSERT_EQ(ErrorCodes::KeyNotFound, swKey.getStatus());
    }

    KeysCollectionDocument origKey1(1);
    origKey1.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey1.toBSON()));

    KeysCollectionDocument origKey2(2);
    origKey2.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(110, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey2.toBSON()));

    {
        auto refreshStatus = cache.refresh(operationContext());
        ASSERT_OK(refreshStatus.getStatus());

        auto key = refreshStatus.getValue();
        ASSERT_EQ(2, key.getKeyId());
        ASSERT_EQ(origKey2.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(110, 0), key.getExpiresAt().asTimestamp());
    }

    {
        auto swKey = cache.getInternalKey(LogicalTime(Timestamp(108, 1)));
        ASSERT_OK(swKey.getStatus());

        auto key = swKey.getValue();
        ASSERT_EQ(2, key.getKeyId());
        ASSERT_EQ(origKey2.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(110, 0), key.getExpiresAt().asTimestamp());
    }
}

TEST_F(CacheTest, RefreshCanIncrementallyGetNewKeysShardedClient) {
    testGetRefreshCanIncrementallyGetNewKeys(catalogClient());
}

TEST_F(CacheTest, RefreshCanIncrementallyGetNewKeysDirectClient) {
    testGetRefreshCanIncrementallyGetNewKeys(directClient());
}

void CacheTest::testCacheExternalKeyBasic(KeysCollectionClient* client) {
    KeysCollectionCache cache("test", client);

    auto swExternalKeys = cache.getExternalKeysById(5, LogicalTime(Timestamp(10, 1)));
    ASSERT_EQ(ErrorCodes::KeyNotFound, swExternalKeys.getStatus());

    ExternalKeysCollectionDocument externalKey(OID::gen(), 5);
    externalKey.setTTLExpiresAt(getServiceContext()->getFastClockSource()->now() + Seconds(30));
    externalKey.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(100, 0))});

    cache.cacheExternalKey(externalKey);

    swExternalKeys = cache.getExternalKeysById(5, LogicalTime(Timestamp(10, 1)));
    ASSERT_OK(swExternalKeys.getStatus());
    ASSERT_EQ(1, swExternalKeys.getValue().size());

    auto cachedKey = swExternalKeys.getValue().front();
    ASSERT_EQ(externalKey.getId(), cachedKey.getId());
    ASSERT_EQ(externalKey.getPurpose(), cachedKey.getPurpose());
    ASSERT_EQ(externalKey.getExpiresAt().asTimestamp(), cachedKey.getExpiresAt().asTimestamp());
    ASSERT_EQ(*externalKey.getTTLExpiresAt(), *cachedKey.getTTLExpiresAt());
}

TEST_F(CacheTest, CacheExternalKeyBasicShardedClient) {
    testCacheExternalKeyBasic(catalogClient());
}

TEST_F(CacheTest, CacheExternalKeyBasicDirectClient) {
    testCacheExternalKeyBasic(directClient());
}

void CacheTest::testRefreshClearsRemovedExternalKeys(KeysCollectionClient* client) {
    KeysCollectionCache cache("test", directClient());

    KeysCollectionDocument origKey0(1);
    origKey0.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    insertDocument(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey0.toBSON());

    ExternalKeysCollectionDocument origKey1(OID::gen(), 1);
    origKey1.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    origKey1.setTTLExpiresAt(getServiceContext()->getFastClockSource()->now() + Seconds(30));
    insertDocument(
        operationContext(), NamespaceString::kExternalKeysCollectionNamespace, origKey1.toBSON());

    ExternalKeysCollectionDocument origKey2(OID::gen(), 1);
    origKey2.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(205, 0))});
    insertDocument(
        operationContext(), NamespaceString::kExternalKeysCollectionNamespace, origKey2.toBSON());

    // After a refresh, both keys should be in the cache.
    {
        auto refreshStatus = cache.refresh(operationContext());
        ASSERT_OK(refreshStatus.getStatus());

        auto swExternalKeys = cache.getExternalKeysById(1, LogicalTime(Timestamp(1, 0)));
        ASSERT_OK(swExternalKeys.getStatus());
        ASSERT_EQ(2, swExternalKeys.getValue().size());
    }

    // After a key is deleted from the underlying collection, the next refresh should remove it from
    // the cache.
    deleteDocument(
        operationContext(), NamespaceString::kExternalKeysCollectionNamespace, origKey1.toBSON());

    // The key is still cached until refresh.
    {
        auto swExternalKeys = cache.getExternalKeysById(1, LogicalTime(Timestamp(1, 0)));
        ASSERT_OK(swExternalKeys.getStatus());
        ASSERT_EQ(2, swExternalKeys.getValue().size());
    }

    {
        auto refreshStatus = cache.refresh(operationContext());
        ASSERT_OK(refreshStatus.getStatus());

        // Now the key is no longer cached.
        auto swExternalKeys = cache.getExternalKeysById(1, LogicalTime(Timestamp(1, 0)));
        ASSERT_OK(swExternalKeys.getStatus());
        ASSERT_EQ(1, swExternalKeys.getValue().size());
        auto key = swExternalKeys.getValue().front();

        ASSERT_EQ(origKey2.getId(), key.getId());
        ASSERT_EQ(origKey2.getPurpose(), key.getPurpose());
        ASSERT_EQ(origKey2.getExpiresAt().asTimestamp(), key.getExpiresAt().asTimestamp());
        ASSERT(!key.getTTLExpiresAt());
    }

    // Remove the final key and the external keys cache should be empty.
    deleteDocument(
        operationContext(), NamespaceString::kExternalKeysCollectionNamespace, origKey2.toBSON());

    {
        auto refreshStatus = cache.refresh(operationContext());
        ASSERT_OK(refreshStatus.getStatus());

        auto swExternalKeys = cache.getExternalKeysById(1, LogicalTime(Timestamp(1, 0)));
        ASSERT_EQ(ErrorCodes::KeyNotFound, swExternalKeys.getStatus());
    }
}

TEST_F(CacheTest, RefreshClearsRemovedExternalKeysShardedClient) {
    testRefreshClearsRemovedExternalKeys(catalogClient());
}

TEST_F(CacheTest, RefreshClearsRemovedExternalKeysDirectClient) {
    testRefreshClearsRemovedExternalKeys(directClient());
}

void CacheTest::testRefreshHandlesKeysReceivingTTLValue(KeysCollectionClient* client) {
    KeysCollectionCache cache("test", client);

    KeysCollectionDocument origKey0(1);
    origKey0.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    insertDocument(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey0.toBSON());

    ExternalKeysCollectionDocument origKey1(OID::gen(), 1);
    origKey1.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    insertDocument(
        operationContext(), NamespaceString::kExternalKeysCollectionNamespace, origKey1.toBSON());

    // Refresh and the external key should be in the cache.
    {
        auto refreshStatus = cache.refresh(operationContext());
        ASSERT_OK(refreshStatus.getStatus());

        auto swExternalKeys = cache.getExternalKeysById(1, LogicalTime(Timestamp(1, 0)));
        ASSERT_OK(swExternalKeys.getStatus());
        ASSERT_EQ(1, swExternalKeys.getValue().size());
        auto key = swExternalKeys.getValue().front();

        ASSERT_EQ(origKey1.getId(), key.getId());
        ASSERT_EQ(origKey1.getPurpose(), key.getPurpose());
        ASSERT_EQ(origKey1.getExpiresAt().asTimestamp(), key.getExpiresAt().asTimestamp());
        ASSERT(!key.getTTLExpiresAt());
    }

    origKey1.setTTLExpiresAt(getServiceContext()->getFastClockSource()->now() + Seconds(30));
    updateDocument(operationContext(),
                   NamespaceString::kExternalKeysCollectionNamespace,
                   BSON(ExternalKeysCollectionDocument::kIdFieldName << origKey1.getId()),
                   origKey1.toBSON());

    // Refresh and the external key should have been updated.
    {
        auto refreshStatus = cache.refresh(operationContext());
        ASSERT_OK(refreshStatus.getStatus());

        auto swExternalKeys = cache.getExternalKeysById(1, LogicalTime(Timestamp(1, 0)));
        ASSERT_OK(swExternalKeys.getStatus());
        ASSERT_EQ(1, swExternalKeys.getValue().size());
        auto key = swExternalKeys.getValue().front();

        ASSERT_EQ(origKey1.getId(), key.getId());
        ASSERT_EQ(origKey1.getPurpose(), key.getPurpose());
        ASSERT_EQ(origKey1.getExpiresAt().asTimestamp(), key.getExpiresAt().asTimestamp());
        ASSERT(key.getTTLExpiresAt());
        ASSERT_EQ(*origKey1.getTTLExpiresAt(), *key.getTTLExpiresAt());
    }
}

TEST_F(CacheTest, RefreshHandlesKeysReceivingTTLValueShardedClient) {
    testRefreshHandlesKeysReceivingTTLValue(catalogClient());
}

TEST_F(CacheTest, RefreshHandlesKeysReceivingTTLValueDirectClient) {
    testRefreshHandlesKeysReceivingTTLValue(directClient());
}

TEST_F(CacheTest, ResetCacheShouldNotClearKeysIfMajorityReadsAreSupported) {
    auto directClient = std::make_unique<KeysCollectionClientDirect>(false /* mustUseLocalReads */);
    KeysCollectionCache cache("test", directClient.get());

    KeysCollectionDocument origKey0(1);
    origKey0.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    insertDocument(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey0.toBSON());

    ASSERT_OK(cache.refresh(operationContext()));

    auto swInternalKey = cache.getInternalKey(LogicalTime(Timestamp(1, 0)));
    ASSERT_OK(swInternalKey.getStatus());
    ASSERT_EQ(1, swInternalKey.getValue().getKeyId());

    // Resetting the cache shouldn't remove the key since the client supports majority reads.
    cache.resetCache();
    swInternalKey = cache.getInternalKey(LogicalTime(Timestamp(1, 0)));
    ASSERT_OK(swInternalKey.getStatus());
    ASSERT_EQ(1, swInternalKey.getValue().getKeyId());
}

TEST_F(CacheTest, ResetCacheShouldClearKeysIfMajorityReadsAreNotSupported) {
    auto directClient = std::make_unique<KeysCollectionClientDirect>(true /* mustUseLocalReads */);
    KeysCollectionCache cache("test", directClient.get());

    KeysCollectionDocument origKey0(1);
    origKey0.setKeysCollectionDocumentBase(
        {"test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    insertDocument(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey0.toBSON());

    ASSERT_OK(cache.refresh(operationContext()));

    auto swInternalKey = cache.getInternalKey(LogicalTime(Timestamp(1, 0)));
    ASSERT_OK(swInternalKey.getStatus());
    ASSERT_EQ(1, swInternalKey.getValue().getKeyId());

    // Resetting the cache should remove the key since the client does not support majority reads.
    cache.resetCache();
    swInternalKey = cache.getInternalKey(LogicalTime(Timestamp(1, 0)));
    ASSERT_EQ(ErrorCodes::KeyNotFound, swInternalKey);
}

}  // namespace
}  // namespace mongo
