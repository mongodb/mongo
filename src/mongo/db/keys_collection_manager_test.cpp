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
#include "mongo/db/keys_collection_document.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

class KeysManagerTest : public ConfigServerTestFixture {
public:
    KeysCollectionManager* keyManager() {
        return _keyManager.get();
    }

protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();

        auto clockSource = stdx::make_unique<ClockSourceMock>();
        operationContext()->getServiceContext()->setFastClockSource(std::move(clockSource));
        auto catalogClient = Grid::get(operationContext())->catalogClient(operationContext());
        _keyManager = stdx::make_unique<KeysCollectionManager>("dummy", catalogClient, Seconds(1));
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
    std::unique_ptr<KeysCollectionManager> _keyManager;
};

TEST_F(KeysManagerTest, GetKeyForValidationTimesOutIfRefresherIsNotRunning) {
    operationContext()->setDeadlineAfterNowBy(Microseconds(250 * 1000));

    ASSERT_THROWS(
        keyManager()->getKeyForValidation(operationContext(), 1, LogicalTime(Timestamp(100, 0))),
        DBException);
}

TEST_F(KeysManagerTest, GetKeyForValidationErrorsIfKeyDoesntExist) {
    keyManager()->startMonitoring(getServiceContext());

    auto keyStatus =
        keyManager()->getKeyForValidation(operationContext(), 1, LogicalTime(Timestamp(100, 0)));
    ASSERT_EQ(ErrorCodes::KeyNotFound, keyStatus.getStatus());
}

TEST_F(KeysManagerTest, GetKeyShouldReturnRightKey) {
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

TEST_F(KeysManagerTest, GetKeyShouldErrorIfKeyIdMismatchKey) {
    keyManager()->startMonitoring(getServiceContext());

    KeysCollectionDocument origKey1(
        1, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));

    auto keyStatus =
        keyManager()->getKeyForValidation(operationContext(), 2, LogicalTime(Timestamp(100, 0)));
    ASSERT_EQ(ErrorCodes::KeyNotFound, keyStatus.getStatus());
}

TEST_F(KeysManagerTest, GetKeyWithoutRefreshShouldReturnRightKey) {
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

TEST_F(KeysManagerTest, GetKeyForSigningTimesOutIfRefresherIsNotRunning) {
    operationContext()->setDeadlineAfterNowBy(Microseconds(250 * 1000));

    ASSERT_THROWS(
        keyManager()->getKeyForSigning(operationContext(), LogicalTime(Timestamp(100, 0))),
        DBException);
}

TEST_F(KeysManagerTest, GetKeyForSigningTimesOutIfKeyDoesntExist) {
    keyManager()->startMonitoring(getServiceContext());

    operationContext()->setDeadlineAfterNowBy(Microseconds(250 * 1000));

    ASSERT_THROWS(
        keyManager()->getKeyForSigning(operationContext(), LogicalTime(Timestamp(100, 0))),
        DBException);
}

TEST_F(KeysManagerTest, GetKeyForSigningShouldReturnRightKey) {
    keyManager()->startMonitoring(getServiceContext());

    KeysCollectionDocument origKey1(
        1, "dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0)));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString(KeysCollectionDocument::ConfigNS), origKey1.toBSON()));

    auto keyStatus =
        keyManager()->getKeyForSigning(operationContext(), LogicalTime(Timestamp(100, 0)));
    ASSERT_OK(keyStatus.getStatus());

    auto key = keyStatus.getValue();
    ASSERT_EQ(1, key.getKeyId());
    ASSERT_EQ(origKey1.getKey(), key.getKey());
    ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
}

TEST_F(KeysManagerTest, GetKeyForSigningWithoutRefreshShouldReturnRightKey) {
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
        auto keyStatus =
            keyManager()->getKeyForSigning(operationContext(), LogicalTime(Timestamp(100, 0)));
        ASSERT_OK(keyStatus.getStatus());

        auto key = keyStatus.getValue();
        ASSERT_EQ(1, key.getKeyId());
        ASSERT_EQ(origKey1.getKey(), key.getKey());
        ASSERT_EQ(Timestamp(105, 0), key.getExpiresAt().asTimestamp());
    }

    {
        auto keyStatus =
            keyManager()->getKeyForSigning(operationContext(), LogicalTime(Timestamp(105, 0)));
        ASSERT_OK(keyStatus.getStatus());

        auto key = keyStatus.getValue();
        ASSERT_EQ(2, key.getKeyId());
        ASSERT_EQ(origKey2.getKey(), key.getKey());
        ASSERT_EQ(Timestamp(110, 0), key.getExpiresAt().asTimestamp());
    }
}

TEST_F(KeysManagerTest, ShouldCreateKeysIfKeyGeneratorEnabled) {
    keyManager()->startMonitoring(getServiceContext());

    const LogicalTime currentTime(LogicalTime(Timestamp(100, 0)));
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(currentTime);

    keyManager()->enableKeyGenerator(operationContext(), true);

    auto keyStatus =
        keyManager()->getKeyForSigning(operationContext(), LogicalTime(Timestamp(100, 100)));
    ASSERT_OK(keyStatus.getStatus());

    auto key = keyStatus.getValue();
    ASSERT_EQ(Timestamp(101, 0), key.getExpiresAt().asTimestamp());
}

TEST_F(KeysManagerTest, EnableModeFlipFlopStressTest) {
    keyManager()->startMonitoring(getServiceContext());

    const LogicalTime currentTime(LogicalTime(Timestamp(100, 0)));
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(currentTime);

    bool doEnable = true;

    for (int x = 0; x < 10; x++) {
        keyManager()->enableKeyGenerator(operationContext(), doEnable);

        auto keyStatus =
            keyManager()->getKeyForSigning(operationContext(), LogicalTime(Timestamp(100, 100)));
        ASSERT_OK(keyStatus.getStatus());

        auto key = keyStatus.getValue();
        ASSERT_EQ(Timestamp(101, 0), key.getExpiresAt().asTimestamp());

        doEnable = !doEnable;
    }
}

TEST_F(KeysManagerTest, ShouldStillBeAbleToUpdateCacheEvenIfItCantCreateKeys) {
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

}  // namespace mongo
