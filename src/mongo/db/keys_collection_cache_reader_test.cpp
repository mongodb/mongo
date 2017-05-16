/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/keys_collection_cache_reader.h"
#include "mongo/db/keys_collection_document.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/s/grid.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using CacheReaderTest = ConfigServerTestFixture;

TEST_F(CacheReaderTest, ErrorsIfCacheIsEmpty) {
    auto catalogClient = Grid::get(operationContext())->catalogClient(operationContext());
    KeysCollectionCacheReader reader("test", catalogClient);
    auto status = reader.getKey(LogicalTime(Timestamp(1, 0))).getStatus();
    ASSERT_EQ(ErrorCodes::KeyNotFound, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(CacheReaderTest, RefreshErrorsIfCacheIsEmpty) {
    auto catalogClient = Grid::get(operationContext())->catalogClient(operationContext());
    KeysCollectionCacheReader reader("test", catalogClient);
    auto status = reader.refresh(operationContext()).getStatus();
    ASSERT_EQ(ErrorCodes::KeyNotFound, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(CacheReaderTest, GetKeyShouldReturnCorrectKeyAfterRefresh) {
    auto catalogClient = Grid::get(operationContext())->catalogClient(operationContext());
    KeysCollectionCacheReader reader("test", catalogClient);

    KeysCollectionDocument origKey1(
        1, "test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));

    auto refreshStatus = reader.refresh(operationContext());
    ASSERT_OK(refreshStatus.getStatus());

    {
        auto key = refreshStatus.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }

    auto status = reader.getKey(LogicalTime(Timestamp(1, 0)));
    ASSERT_OK(status.getStatus());

    {
        auto key = status.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }
}

TEST_F(CacheReaderTest, GetKeyShouldReturnErrorIfNoKeyIsValidForGivenTime) {
    auto catalogClient = Grid::get(operationContext())->catalogClient(operationContext());
    KeysCollectionCacheReader reader("test", catalogClient);

    KeysCollectionDocument origKey1(
        1, "test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));

    auto refreshStatus = reader.refresh(operationContext());
    ASSERT_OK(refreshStatus.getStatus());

    {
        auto key = refreshStatus.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }

    auto status = reader.getKey(LogicalTime(Timestamp(110, 0)));
    ASSERT_EQ(ErrorCodes::KeyNotFound, status.getStatus());
}

TEST_F(CacheReaderTest, GetKeyShouldReturnOldestKeyPossible) {
    auto catalogClient = Grid::get(operationContext())->catalogClient(operationContext());
    KeysCollectionCacheReader reader("test", catalogClient);

    KeysCollectionDocument origKey0(
        0, "test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(100, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey0.toBSON()));

    KeysCollectionDocument origKey1(
        1, "test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));

    KeysCollectionDocument origKey2(
        2, "test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(110, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey2.toBSON()));

    auto refreshStatus = reader.refresh(operationContext());
    ASSERT_OK(refreshStatus.getStatus());

    {
        auto key = refreshStatus.getValue();
        ASSERT_EQ(2, key.getKeyId());
        ASSERT_EQ(origKey2.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(110, 0), key.getExpiresAt().asTimestamp());
    }

    auto keyStatus = reader.getKey(LogicalTime(Timestamp(103, 1)));
    ASSERT_OK(keyStatus.getStatus());

    {
        auto key = keyStatus.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }
}

TEST_F(CacheReaderTest, RefreshShouldNotGetKeysForOtherPurpose) {
    auto catalogClient = Grid::get(operationContext())->catalogClient(operationContext());
    KeysCollectionCacheReader reader("test", catalogClient);

    KeysCollectionDocument origKey0(
        0, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(100, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey0.toBSON()));

    {
        auto refreshStatus = reader.refresh(operationContext());
        ASSERT_EQ(ErrorCodes::KeyNotFound, refreshStatus.getStatus());

        auto emptyKeyStatus = reader.getKey(LogicalTime(Timestamp(50, 0)));
        ASSERT_EQ(ErrorCodes::KeyNotFound, emptyKeyStatus.getStatus());
    }

    KeysCollectionDocument origKey1(
        1, "test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));

    {
        auto refreshStatus = reader.refresh(operationContext());
        ASSERT_OK(refreshStatus.getStatus());

        auto key = refreshStatus.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }

    auto keyStatus = reader.getKey(LogicalTime(Timestamp(60, 1)));
    ASSERT_OK(keyStatus.getStatus());

    {
        auto key = keyStatus.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }
}

TEST_F(CacheReaderTest, RefreshCanIncrementallyGetNewKeys) {
    auto catalogClient = Grid::get(operationContext())->catalogClient(operationContext());
    KeysCollectionCacheReader reader("test", catalogClient);

    KeysCollectionDocument origKey0(
        0, "test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(100, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey0.toBSON()));

    {
        auto refreshStatus = reader.refresh(operationContext());
        ASSERT_OK(refreshStatus.getStatus());


        auto key = refreshStatus.getValue();
        ASSERT_EQ(0, key.getKeyId());
        ASSERT_EQ(origKey0.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(100, 0), key.getExpiresAt().asTimestamp());

        auto keyStatus = reader.getKey(LogicalTime(Timestamp(112, 1)));
        ASSERT_EQ(ErrorCodes::KeyNotFound, keyStatus.getStatus());
    }

    KeysCollectionDocument origKey1(
        1, "test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));

    KeysCollectionDocument origKey2(
        2, "test", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(110, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey2.toBSON()));

    {
        auto refreshStatus = reader.refresh(operationContext());
        ASSERT_OK(refreshStatus.getStatus());

        auto key = refreshStatus.getValue();
        ASSERT_EQ(2, key.getKeyId());
        ASSERT_EQ(origKey2.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(110, 0), key.getExpiresAt().asTimestamp());
    }

    {
        auto keyStatus = reader.getKey(LogicalTime(Timestamp(108, 1)));

        auto key = keyStatus.getValue();
        ASSERT_EQ(2, key.getKeyId());
        ASSERT_EQ(origKey2.getKey(), key.getKey());
        ASSERT_EQ("test", key.getPurpose());
        ASSERT_EQ(Timestamp(110, 0), key.getExpiresAt().asTimestamp());
    }
}

}  // namespace mongo
