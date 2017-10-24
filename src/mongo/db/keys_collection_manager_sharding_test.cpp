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

#include <set>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_document.h"
#include "mongo/db/keys_collection_manager_sharding.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

class KeysManagerShardedTest : public ConfigServerTestFixture {
public:
    KeysCollectionManagerSharding* keyManager() {
        return _keyManager.get();
    }

protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();

        serverGlobalParams.featureCompatibility.setVersion(
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36);
        serverGlobalParams.validateFeaturesAsMaster.store(true);

        auto clockSource = stdx::make_unique<ClockSourceMock>();
        // Timestamps of "0 seconds" are not allowed, so we must advance our clock mock to the first
        // real second.
        clockSource->advance(Seconds(1));

        operationContext()->getServiceContext()->setFastClockSource(std::move(clockSource));
        auto catalogClient = stdx::make_unique<KeysCollectionClientSharded>(
            Grid::get(operationContext())->catalogClient());
        _keyManager = stdx::make_unique<KeysCollectionManagerSharding>(
            "dummy", std::move(catalogClient), Seconds(1));
    }

    void tearDown() override {
        _keyManager->stopMonitoring();

        ConfigServerTestFixture::tearDown();
    }

    std::unique_ptr<DistLockManager> makeDistLockManager(
        std::unique_ptr<DistLockCatalog> distLockCatalog) override {
        invariant(distLockCatalog);
        return stdx::make_unique<DistLockManagerMock>(std::move(distLockCatalog));
    }

private:
    std::unique_ptr<KeysCollectionManagerSharding> _keyManager;
};

TEST_F(KeysManagerShardedTest, GetKeyForValidationTimesOutIfRefresherIsNotRunning) {
    operationContext()->setDeadlineAfterNowBy(Microseconds(250 * 1000));

    ASSERT_THROWS(keyManager()
                      ->getKeyForValidation(operationContext(), 1, LogicalTime(Timestamp(100, 0)))
                      .status_with_transitional_ignore(),
                  DBException);
}

TEST_F(KeysManagerShardedTest, GetKeyForValidationErrorsIfKeyDoesntExist) {
    keyManager()->startMonitoring(getServiceContext());

    auto keyStatus =
        keyManager()->getKeyForValidation(operationContext(), 1, LogicalTime(Timestamp(100, 0)));
    ASSERT_EQ(ErrorCodes::KeyNotFound, keyStatus.getStatus());
}

TEST_F(KeysManagerShardedTest, GetKeyWithSingleKey) {
    keyManager()->startMonitoring(getServiceContext());

    KeysCollectionDocument origKey1(
        1, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));

    auto keyStatus =
        keyManager()->getKeyForValidation(operationContext(), 1, LogicalTime(Timestamp(100, 0)));
    ASSERT_OK(keyStatus.getStatus());

    auto key = keyStatus.getValue();
    ASSERT_EQ(1, key.getKeyId());
    ASSERT_EQ(origKey1.getKey(), key.getKey());
    ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
}

TEST_F(KeysManagerShardedTest, GetKeyWithMultipleKeys) {
    keyManager()->startMonitoring(getServiceContext());

    KeysCollectionDocument origKey1(
        1, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));

    KeysCollectionDocument origKey2(
        2, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(205, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey2.toBSON()));

    auto keyStatus =
        keyManager()->getKeyForValidation(operationContext(), 1, LogicalTime(Timestamp(100, 0)));
    ASSERT_OK(keyStatus.getStatus());

    auto key = keyStatus.getValue();
    ASSERT_EQ(1, key.getKeyId());
    ASSERT_EQ(origKey1.getKey(), key.getKey());
    ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());

    keyStatus =
        keyManager()->getKeyForValidation(operationContext(), 2, LogicalTime(Timestamp(100, 0)));
    ASSERT_OK(keyStatus.getStatus());

    key = keyStatus.getValue();
    ASSERT_EQ(2, key.getKeyId());
    ASSERT_EQ(origKey2.getKey(), key.getKey());
    ASSERT_EQ(Timestamp(205, 0), key.getExpiresAt().asTimestamp());
}

TEST_F(KeysManagerShardedTest, GetKeyShouldErrorIfKeyIdMismatchKey) {
    keyManager()->startMonitoring(getServiceContext());

    KeysCollectionDocument origKey1(
        1, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));

    auto keyStatus =
        keyManager()->getKeyForValidation(operationContext(), 2, LogicalTime(Timestamp(100, 0)));
    ASSERT_EQ(ErrorCodes::KeyNotFound, keyStatus.getStatus());
}

TEST_F(KeysManagerShardedTest, GetKeyWithoutRefreshShouldReturnRightKey) {
    keyManager()->startMonitoring(getServiceContext());

    KeysCollectionDocument origKey1(
        1, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));
    KeysCollectionDocument origKey2(
        2, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(110, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey2.toBSON()));

    {
        auto keyStatus = keyManager()->getKeyForValidation(
            operationContext(), 1, LogicalTime(Timestamp(100, 0)));
        ASSERT_OK(keyStatus.getStatus());

        auto key = keyStatus.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }

    {
        auto keyStatus = keyManager()->getKeyForValidation(
            operationContext(), 2, LogicalTime(Timestamp(105, 0)));
        ASSERT_OK(keyStatus.getStatus());

        auto key = keyStatus.getValue();
        ASSERT_EQ(2, key.getKeyId());
        ASSERT_EQ(origKey2.getKey(), key.getKey());
        ASSERT_EQ(Timestamp(110, 0), key.getExpiresAt().asTimestamp());
    }
}

TEST_F(KeysManagerShardedTest, GetKeyForSigningShouldReturnRightKey) {
    keyManager()->startMonitoring(getServiceContext());

    KeysCollectionDocument origKey1(
        1, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));

    keyManager()->refreshNow(operationContext());

    auto keyStatus = keyManager()->getKeyForSigning(nullptr, LogicalTime(Timestamp(100, 0)));
    ASSERT_OK(keyStatus.getStatus());

    auto key = keyStatus.getValue();
    ASSERT_EQ(1, key.getKeyId());
    ASSERT_EQ(origKey1.getKey(), key.getKey());
    ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
}

TEST_F(KeysManagerShardedTest, GetKeyForSigningShouldReturnRightOldKey) {
    keyManager()->startMonitoring(getServiceContext());

    KeysCollectionDocument origKey1(
        1, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));
    KeysCollectionDocument origKey2(
        2, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(110, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey2.toBSON()));

    keyManager()->refreshNow(operationContext());

    {
        auto keyStatus = keyManager()->getKeyForSigning(nullptr, LogicalTime(Timestamp(100, 0)));
        ASSERT_OK(keyStatus.getStatus());

        auto key = keyStatus.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }

    {
        auto keyStatus = keyManager()->getKeyForSigning(nullptr, LogicalTime(Timestamp(105, 0)));
        ASSERT_OK(keyStatus.getStatus());

        auto key = keyStatus.getValue();
        ASSERT_EQ(2, key.getKeyId());
        ASSERT_EQ(origKey2.getKey(), key.getKey());
        ASSERT_EQ(Timestamp(110, 0), key.getExpiresAt().asTimestamp());
    }
}

TEST_F(KeysManagerShardedTest, ShouldCreateKeysIfKeyGeneratorEnabled) {
    keyManager()->startMonitoring(getServiceContext());

    const LogicalTime currentTime(LogicalTime(Timestamp(100, 0)));
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(currentTime);

    keyManager()->enableKeyGenerator(operationContext(), true);
    keyManager()->refreshNow(operationContext());

    auto keyStatus = keyManager()->getKeyForSigning(nullptr, LogicalTime(Timestamp(100, 100)));
    ASSERT_OK(keyStatus.getStatus());

    auto key = keyStatus.getValue();
    ASSERT_EQ(Timestamp(101, 0), key.getExpiresAt().asTimestamp());
}

TEST_F(KeysManagerShardedTest, EnableModeFlipFlopStressTest) {
    keyManager()->startMonitoring(getServiceContext());

    const LogicalTime currentTime(LogicalTime(Timestamp(100, 0)));
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(currentTime);

    bool doEnable = true;

    for (int x = 0; x < 10; x++) {
        keyManager()->enableKeyGenerator(operationContext(), doEnable);
        keyManager()->refreshNow(operationContext());

        auto keyStatus = keyManager()->getKeyForSigning(nullptr, LogicalTime(Timestamp(100, 100)));
        ASSERT_OK(keyStatus.getStatus());

        auto key = keyStatus.getValue();
        ASSERT_EQ(Timestamp(101, 0), key.getExpiresAt().asTimestamp());

        doEnable = !doEnable;
    }
}

TEST_F(KeysManagerShardedTest, ShouldStillBeAbleToUpdateCacheEvenIfItCantCreateKeys) {
    KeysCollectionDocument origKey1(
        1, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));

    // Set the time to be very ahead so the updater will be forced to create new keys.
    const LogicalTime fakeTime(Timestamp(20000, 0));
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(fakeTime);

    FailPointEnableBlock failWriteBlock("failCollectionInserts");

    {
        FailPointEnableBlock failQueryBlock("planExecutorAlwaysFails");
        keyManager()->startMonitoring(getServiceContext());
        keyManager()->enableKeyGenerator(operationContext(), true);
    }

    auto keyStatus =
        keyManager()->getKeyForValidation(operationContext(), 1, LogicalTime(Timestamp(100, 0)));
    ASSERT_OK(keyStatus.getStatus());

    auto key = keyStatus.getValue();
    ASSERT_EQ(1, key.getKeyId());
    ASSERT_EQ(origKey1.getKey(), key.getKey());
    ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
}

TEST_F(KeysManagerShardedTest, ShouldNotCreateKeysWithDisableKeyGenerationFailPoint) {
    const LogicalTime currentTime(Timestamp(100, 0));
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(currentTime);

    {
        FailPointEnableBlock failKeyGenerationBlock("disableKeyGeneration");
        keyManager()->startMonitoring(getServiceContext());
        keyManager()->enableKeyGenerator(operationContext(), true);

        keyManager()->refreshNow(operationContext());
        auto keyStatus = keyManager()->getKeyForValidation(
            operationContext(), 1, LogicalTime(Timestamp(100, 0)));
        ASSERT_EQ(ErrorCodes::KeyNotFound, keyStatus.getStatus());
    }

    // Once the failpoint is disabled, the generator can make keys again.
    keyManager()->refreshNow(operationContext());
    auto keyStatus = keyManager()->getKeyForSigning(nullptr, LogicalTime(Timestamp(100, 0)));
    ASSERT_OK(keyStatus.getStatus());
}

TEST_F(KeysManagerShardedTest, HasSeenKeysIsFalseUntilKeysAreFound) {
    const LogicalTime currentTime(Timestamp(100, 0));
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(currentTime);

    ASSERT_EQ(false, keyManager()->hasSeenKeys());

    {
        FailPointEnableBlock failKeyGenerationBlock("disableKeyGeneration");
        keyManager()->startMonitoring(getServiceContext());
        keyManager()->enableKeyGenerator(operationContext(), true);

        keyManager()->refreshNow(operationContext());
        auto keyStatus = keyManager()->getKeyForValidation(
            operationContext(), 1, LogicalTime(Timestamp(100, 0)));
        ASSERT_EQ(ErrorCodes::KeyNotFound, keyStatus.getStatus());

        ASSERT_EQ(false, keyManager()->hasSeenKeys());
    }

    // Once the failpoint is disabled, the generator can make keys again.
    keyManager()->refreshNow(operationContext());
    auto keyStatus = keyManager()->getKeyForSigning(nullptr, LogicalTime(Timestamp(100, 0)));
    ASSERT_OK(keyStatus.getStatus());

    ASSERT_EQ(true, keyManager()->hasSeenKeys());
}

TEST_F(KeysManagerShardedTest, ShouldNotReturnKeysInFeatureCompatibilityVersion34) {
    serverGlobalParams.featureCompatibility.setVersion(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo34);

    keyManager()->startMonitoring(getServiceContext());
    keyManager()->enableKeyGenerator(operationContext(), true);

    KeysCollectionDocument origKey(
        1, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey.toBSON()));

    keyManager()->refreshNow(operationContext());

    auto keyStatus =
        keyManager()->getKeyForValidation(operationContext(), 1, LogicalTime(Timestamp(100, 0)));
    ASSERT_EQ(ErrorCodes::KeyNotFound, keyStatus.getStatus());
}

}  // namespace mongo
