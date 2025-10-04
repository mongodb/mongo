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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/db/key_generator.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"

#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace mongo {
namespace {

class KeyGeneratorUpdateTest : public ConfigServerTestFixture {
protected:
    KeyGeneratorUpdateTest() : ConfigServerTestFixture(Options{}.useMockClock(true)) {}

    void setUp() override {
        ConfigServerTestFixture::setUp();

        _catalogClient = std::make_unique<KeysCollectionClientSharded>(
            Grid::get(operationContext())->catalogClient());
    }

    KeysCollectionClient* catalogClient() const {
        return _catalogClient.get();
    }

private:
    std::unique_ptr<KeysCollectionClient> _catalogClient;
};

TEST_F(KeyGeneratorUpdateTest, ShouldCreate2KeysFromEmpty) {
    KeyGenerator generator("dummy", catalogClient(), Seconds(5));

    const LogicalTime currentTime(LogicalTime(Timestamp(100, 2)));
    VectorClockMutable::get(operationContext())->tickClusterTimeTo(currentTime);

    auto generateStatus = generator.generateNewKeysIfNeeded(operationContext());
    ASSERT_OK(generateStatus);

    auto allKeys = getKeys(operationContext());

    ASSERT_EQ(2u, allKeys.size());

    const auto& key1 = allKeys.front();
    ASSERT_EQ(currentTime.asTimestamp().asLL(), key1.getKeyId());
    ASSERT_EQ("dummy", key1.getPurpose());
    ASSERT_EQ(Timestamp(105, 0), key1.getExpiresAt().asTimestamp());

    const auto& key2 = allKeys.back();
    ASSERT_EQ(currentTime.asTimestamp().asLL() + 1, key2.getKeyId());
    ASSERT_EQ("dummy", key2.getPurpose());
    ASSERT_EQ(Timestamp(110, 0), key2.getExpiresAt().asTimestamp());

    ASSERT_NE(key1.getKey(), key2.getKey());
}

TEST_F(KeyGeneratorUpdateTest, ShouldPropagateWriteError) {
    KeyGenerator generator("dummy", catalogClient(), Seconds(5));

    const LogicalTime currentTime(LogicalTime(Timestamp(100, 2)));
    VectorClockMutable::get(operationContext())->tickClusterTimeTo(currentTime);

    FailPointEnableBlock failWriteBlock("failCollectionInserts");

    auto generateStatus = generator.generateNewKeysIfNeeded(operationContext());
    ASSERT_EQ(ErrorCodes::FailPointEnabled, generateStatus);
}

TEST_F(KeyGeneratorUpdateTest, ShouldCreateAnotherKeyIfOnlyOneKeyExists) {
    KeyGenerator generator("dummy", catalogClient(), Seconds(5));

    VectorClockMutable::get(operationContext())->tickClusterTimeTo(LogicalTime(Timestamp(100, 2)));

    KeysCollectionDocument origKey1(1);
    origKey1.setKeysCollectionDocumentBase(
        {"dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey1.toBSON()));

    {
        auto allKeys = getKeys(operationContext());

        ASSERT_EQ(1u, allKeys.size());

        const auto& key1 = allKeys.front();
        ASSERT_EQ(1, key1.getKeyId());
        ASSERT_EQ("dummy", key1.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key1.getExpiresAt().asTimestamp());
    }

    const auto currentTime = VectorClock::get(operationContext())->getTime();
    const auto clusterTime = currentTime.clusterTime();

    auto generateStatus = generator.generateNewKeysIfNeeded(operationContext());
    ASSERT_OK(generateStatus);

    {
        auto allKeys = getKeys(operationContext());

        ASSERT_EQ(2u, allKeys.size());

        const auto& key1 = allKeys.front();
        ASSERT_EQ(1, key1.getKeyId());
        ASSERT_EQ("dummy", key1.getPurpose());
        ASSERT_EQ(origKey1.getKey(), key1.getKey());
        ASSERT_EQ(Timestamp(105, 0), key1.getExpiresAt().asTimestamp());

        const auto& key2 = allKeys.back();
        ASSERT_EQ(clusterTime.asTimestamp().asLL(), key2.getKeyId());
        ASSERT_EQ("dummy", key2.getPurpose());
        ASSERT_EQ(Timestamp(110, 0), key2.getExpiresAt().asTimestamp());

        ASSERT_NE(key1.getKey(), key2.getKey());
    }
}

TEST_F(KeyGeneratorUpdateTest, ShouldCreateAnotherKeyIfNoValidKeyAfterCurrent) {
    KeyGenerator generator("dummy", catalogClient(), Seconds(5));

    VectorClockMutable::get(operationContext())->tickClusterTimeTo(LogicalTime(Timestamp(108, 2)));

    KeysCollectionDocument origKey1(1);
    origKey1.setKeysCollectionDocumentBase(
        {"dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey1.toBSON()));

    KeysCollectionDocument origKey2(2);
    origKey2.setKeysCollectionDocumentBase(
        {"dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(110, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey2.toBSON()));

    {
        auto allKeys = getKeys(operationContext());

        ASSERT_EQ(2u, allKeys.size());

        const auto& key1 = allKeys.front();
        ASSERT_EQ(1, key1.getKeyId());
        ASSERT_EQ("dummy", key1.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key1.getExpiresAt().asTimestamp());

        const auto& key2 = allKeys.back();
        ASSERT_EQ(2, key2.getKeyId());
        ASSERT_EQ("dummy", key2.getPurpose());
        ASSERT_EQ(Timestamp(110, 0), key2.getExpiresAt().asTimestamp());
    }

    const auto currentTime = VectorClock::get(operationContext())->getTime();
    const auto clusterTime = currentTime.clusterTime();

    auto generateStatus = generator.generateNewKeysIfNeeded(operationContext());
    ASSERT_OK(generateStatus);

    auto allKeys = getKeys(operationContext());

    ASSERT_EQ(3u, allKeys.size());

    auto citer = allKeys.cbegin();

    std::set<std::string> seenKeys;

    {
        const auto& key = *citer;
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ("dummy", key.getPurpose());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());

        bool inserted = false;
        std::tie(std::ignore, inserted) = seenKeys.insert(key.getKey().toString());
        ASSERT_TRUE(inserted);
    }

    {
        ++citer;
        const auto& key = *citer;
        ASSERT_EQ(2, key.getKeyId());
        ASSERT_EQ("dummy", key.getPurpose());
        ASSERT_EQ(origKey2.getKey(), key.getKey());
        ASSERT_EQ(Timestamp(110, 0), key.getExpiresAt().asTimestamp());

        bool inserted = false;
        std::tie(std::ignore, inserted) = seenKeys.insert(key.getKey().toString());
        ASSERT_TRUE(inserted);
    }

    {
        ++citer;
        const auto& key = *citer;
        ASSERT_EQ(clusterTime.asTimestamp().asLL(), key.getKeyId());
        ASSERT_EQ("dummy", key.getPurpose());
        ASSERT_EQ(Timestamp(115, 0), key.getExpiresAt().asTimestamp());

        bool inserted = false;
        std::tie(std::ignore, inserted) = seenKeys.insert(key.getKey().toString());
        ASSERT_TRUE(inserted);
    }
}

TEST_F(KeyGeneratorUpdateTest, ShouldCreate2KeysIfAllKeysAreExpired) {
    KeyGenerator generator("dummy", catalogClient(), Seconds(5));

    VectorClockMutable::get(operationContext())->tickClusterTimeTo(LogicalTime(Timestamp(120, 2)));

    KeysCollectionDocument origKey1(1);
    origKey1.setKeysCollectionDocumentBase(
        {"dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey1.toBSON()));

    KeysCollectionDocument origKey2(2);
    origKey2.setKeysCollectionDocumentBase(
        {"dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(110, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey2.toBSON()));

    {
        auto allKeys = getKeys(operationContext());

        ASSERT_EQ(2u, allKeys.size());

        const auto& key1 = allKeys.front();
        ASSERT_EQ(1, key1.getKeyId());
        ASSERT_EQ("dummy", key1.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key1.getExpiresAt().asTimestamp());

        const auto& key2 = allKeys.back();
        ASSERT_EQ(2, key2.getKeyId());
        ASSERT_EQ("dummy", key2.getPurpose());
        ASSERT_EQ(Timestamp(110, 0), key2.getExpiresAt().asTimestamp());
    }

    const auto currentTime = VectorClock::get(operationContext())->getTime();
    const auto clusterTime = currentTime.clusterTime();

    auto generateStatus = generator.generateNewKeysIfNeeded(operationContext());
    ASSERT_OK(generateStatus);

    auto allKeys = getKeys(operationContext());

    ASSERT_EQ(4u, allKeys.size());

    auto citer = allKeys.cbegin();

    std::set<std::string> seenKeys;

    {
        const auto& key = *citer;
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ("dummy", key.getPurpose());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());

        bool inserted = false;
        std::tie(std::ignore, inserted) = seenKeys.insert(key.getKey().toString());
        ASSERT_TRUE(inserted);
    }

    {
        ++citer;
        const auto& key = *citer;
        ASSERT_EQ(2, key.getKeyId());
        ASSERT_EQ("dummy", key.getPurpose());
        ASSERT_EQ(origKey2.getKey(), key.getKey());
        ASSERT_EQ(Timestamp(110, 0), key.getExpiresAt().asTimestamp());

        bool inserted = false;
        std::tie(std::ignore, inserted) = seenKeys.insert(key.getKey().toString());
        ASSERT_TRUE(inserted);
    }

    {
        ++citer;
        const auto& key = *citer;
        ASSERT_EQ(clusterTime.asTimestamp().asLL(), key.getKeyId());
        ASSERT_EQ("dummy", key.getPurpose());
        ASSERT_EQ(Timestamp(125, 0), key.getExpiresAt().asTimestamp());

        bool inserted = false;
        std::tie(std::ignore, inserted) = seenKeys.insert(key.getKey().toString());
        ASSERT_TRUE(inserted);
    }

    {
        ++citer;
        const auto& key = *citer;
        ASSERT_EQ(clusterTime.asTimestamp().asLL() + 1, key.getKeyId());
        ASSERT_EQ("dummy", key.getPurpose());
        ASSERT_NE(origKey1.getKey(), key.getKey());
        ASSERT_NE(origKey2.getKey(), key.getKey());
        ASSERT_EQ(Timestamp(130, 0), key.getExpiresAt().asTimestamp());

        bool inserted = false;
        std::tie(std::ignore, inserted) = seenKeys.insert(key.getKey().toString());
        ASSERT_TRUE(inserted);
    }
}

TEST_F(KeyGeneratorUpdateTest, ShouldNotCreateNewKeyIfThereAre2UnexpiredKeys) {
    KeyGenerator generator("dummy", catalogClient(), Seconds(5));

    const LogicalTime currentTime(LogicalTime(Timestamp(100, 2)));
    VectorClockMutable::get(operationContext())->tickClusterTimeTo(currentTime);

    KeysCollectionDocument origKey1(1);
    origKey1.setKeysCollectionDocumentBase(
        {"dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey1.toBSON()));

    KeysCollectionDocument origKey2(2);
    origKey2.setKeysCollectionDocumentBase(
        {"dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(110, 0))});
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kKeysCollectionNamespace, origKey2.toBSON()));

    {
        auto allKeys = getKeys(operationContext());

        ASSERT_EQ(2u, allKeys.size());

        const auto& key1 = allKeys.front();
        ASSERT_EQ(1, key1.getKeyId());
        ASSERT_EQ("dummy", key1.getPurpose());
        ASSERT_EQ(Timestamp(105, 0), key1.getExpiresAt().asTimestamp());

        const auto& key2 = allKeys.back();
        ASSERT_EQ(2, key2.getKeyId());
        ASSERT_EQ("dummy", key2.getPurpose());
        ASSERT_EQ(Timestamp(110, 0), key2.getExpiresAt().asTimestamp());
    }

    auto generateStatus = generator.generateNewKeysIfNeeded(operationContext());
    ASSERT_OK(generateStatus);

    auto allKeys = getKeys(operationContext());

    ASSERT_EQ(2u, allKeys.size());

    auto citer = allKeys.cbegin();

    {
        const auto& key = *citer;
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ("dummy", key.getPurpose());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }

    {
        ++citer;
        const auto& key = *citer;
        ASSERT_EQ(2, key.getKeyId());
        ASSERT_EQ("dummy", key.getPurpose());
        ASSERT_EQ(origKey2.getKey(), key.getKey());
        ASSERT_EQ(Timestamp(110, 0), key.getExpiresAt().asTimestamp());
    }
}

TEST_F(KeyGeneratorUpdateTest, ShouldNotCreateKeysWithDisableKeyGenerationFailPoint) {
    KeyGenerator generator("dummy", catalogClient(), Seconds(5));

    const LogicalTime currentTime(LogicalTime(Timestamp(100, 0)));
    VectorClockMutable::get(operationContext())->tickClusterTimeTo(currentTime);

    {
        FailPointEnableBlock failKeyGenerationBlock("disableKeyGeneration");

        auto generateStatus = generator.generateNewKeysIfNeeded(operationContext());
        ASSERT_EQ(ErrorCodes::FailPointEnabled, generateStatus);
    }

    auto allKeys = getKeys(operationContext());

    ASSERT_EQ(0U, allKeys.size());
}

}  // namespace
}  // namespace mongo
