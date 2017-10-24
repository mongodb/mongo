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

#include "mongo/bson/timestamp.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/keys_collection_manager_sharding.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/platform/basic.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class LogicalTimeValidatorTest : public ConfigServerTestFixture {
public:
    LogicalTimeValidator* validator() {
        return _validator.get();
    }

protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();

        serverGlobalParams.featureCompatibility.setVersion(
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36);
        serverGlobalParams.validateFeaturesAsMaster.store(true);

        auto clockSource = stdx::make_unique<ClockSourceMock>();
        operationContext()->getServiceContext()->setFastClockSource(std::move(clockSource));
        auto catalogClient = stdx::make_unique<KeysCollectionClientSharded>(
            Grid::get(operationContext())->catalogClient());

        const LogicalTime currentTime(LogicalTime(Timestamp(1, 0)));
        LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(currentTime);

        _keyManager = std::make_shared<KeysCollectionManagerSharding>(
            "dummy", std::move(catalogClient), Seconds(1000));
        _validator = stdx::make_unique<LogicalTimeValidator>(_keyManager);
        _validator->init(operationContext()->getServiceContext());
    }

    void tearDown() override {
        _validator->shutDown();
        ConfigServerTestFixture::tearDown();
    }

    std::unique_ptr<DistLockManager> makeDistLockManager(
        std::unique_ptr<DistLockCatalog> distLockCatalog) override {
        invariant(distLockCatalog);
        return stdx::make_unique<DistLockManagerMock>(std::move(distLockCatalog));
    }

    /**
     * Forces KeyManager to refresh cache and generate new keys.
     */
    void refreshKeyManager() {
        _keyManager->refreshNow(operationContext());
    }

private:
    std::unique_ptr<LogicalTimeValidator> _validator;
    std::shared_ptr<KeysCollectionManagerSharding> _keyManager;
};

TEST_F(LogicalTimeValidatorTest, GetTimeWithIncreasingTimes) {
    validator()->enableKeyGenerator(operationContext(), true);

    LogicalTime t1(Timestamp(10, 0));
    auto newTime = validator()->trySignLogicalTime(t1);

    ASSERT_EQ(t1.asTimestamp(), newTime.getTime().asTimestamp());
    ASSERT_TRUE(newTime.getProof());

    LogicalTime t2(Timestamp(20, 0));
    auto newTime2 = validator()->trySignLogicalTime(t2);

    ASSERT_EQ(t2.asTimestamp(), newTime2.getTime().asTimestamp());
    ASSERT_TRUE(newTime2.getProof());
}

TEST_F(LogicalTimeValidatorTest, ValidateReturnsOkForValidSignature) {
    validator()->enableKeyGenerator(operationContext(), true);

    LogicalTime t1(Timestamp(20, 0));
    refreshKeyManager();
    auto newTime = validator()->trySignLogicalTime(t1);

    ASSERT_OK(validator()->validate(operationContext(), newTime));
}

TEST_F(LogicalTimeValidatorTest, ValidateErrorsOnInvalidTime) {
    validator()->enableKeyGenerator(operationContext(), true);

    LogicalTime t1(Timestamp(20, 0));
    refreshKeyManager();
    auto newTime = validator()->trySignLogicalTime(t1);

    TimeProofService::TimeProof invalidProof = {{{1, 2, 3}}};
    SignedLogicalTime invalidTime(LogicalTime(Timestamp(30, 0)), invalidProof, newTime.getKeyId());
    // ASSERT_THROWS_CODE(validator()->validate(operationContext(), invalidTime), DBException,
    // ErrorCodes::TimeProofMismatch);
    auto status = validator()->validate(operationContext(), invalidTime);
    ASSERT_EQ(ErrorCodes::TimeProofMismatch, status);
}

TEST_F(LogicalTimeValidatorTest, ValidateReturnsOkForValidSignatureWithImplicitRefresh) {
    validator()->enableKeyGenerator(operationContext(), true);

    LogicalTime t1(Timestamp(20, 0));
    auto newTime = validator()->signLogicalTime(operationContext(), t1);

    ASSERT_OK(validator()->validate(operationContext(), newTime));
}

TEST_F(LogicalTimeValidatorTest, ValidateErrorsOnInvalidTimeWithImplicitRefresh) {
    validator()->enableKeyGenerator(operationContext(), true);

    LogicalTime t1(Timestamp(20, 0));
    auto newTime = validator()->signLogicalTime(operationContext(), t1);

    TimeProofService::TimeProof invalidProof = {{{1, 2, 3}}};
    SignedLogicalTime invalidTime(LogicalTime(Timestamp(30, 0)), invalidProof, newTime.getKeyId());
    // ASSERT_THROWS_CODE(validator()->validate(operationContext(), invalidTime), DBException,
    // ErrorCodes::TimeProofMismatch);
    auto status = validator()->validate(operationContext(), invalidTime);
    ASSERT_EQ(ErrorCodes::TimeProofMismatch, status);
}

TEST_F(LogicalTimeValidatorTest, ShouldGossipLogicalTimeIsFalseUntilKeysAreFound) {
    // shouldGossipLogicalTime initially returns false.
    ASSERT_EQ(false, validator()->shouldGossipLogicalTime());

    // shouldGossipLogicalTime still returns false after an unsuccessful refresh.
    refreshKeyManager();

    LogicalTime t1(Timestamp(20, 0));
    validator()->trySignLogicalTime(t1);
    ASSERT_EQ(false, validator()->shouldGossipLogicalTime());

    // Once keys are successfully found, shouldGossipLogicalTime returns true.
    validator()->enableKeyGenerator(operationContext(), true);
    refreshKeyManager();
    auto newTime = validator()->signLogicalTime(operationContext(), t1);

    ASSERT_EQ(true, validator()->shouldGossipLogicalTime());
    ASSERT_OK(validator()->validate(operationContext(), newTime));
}

}  // unnamed namespace
}  // namespace mongo
