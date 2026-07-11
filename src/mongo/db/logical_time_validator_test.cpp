// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/logical_time_validator.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <memory>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class LogicalTimeValidatorTest : public ConfigServerTestFixture {
public:
    LogicalTimeValidator* validator() {
        return _validator.get();
    }

protected:
    LogicalTimeValidatorTest() : ConfigServerTestFixture(Options{}.useMockClock(true)) {}

    void setUp() override {
        ConfigServerTestFixture::setUp();

        auto catalogClient = std::make_unique<KeysCollectionClientSharded>(
            Grid::get(operationContext())->catalogClient());

        const LogicalTime currentTime(LogicalTime(Timestamp(1, 0)));
        VectorClockMutable::get(operationContext())->tickClusterTimeTo(currentTime);

        _keyManager = std::make_shared<KeysCollectionManager>(
            "dummy", std::move(catalogClient), Seconds(1000));
        _validator = std::make_unique<LogicalTimeValidator>(_keyManager);
        _validator->init(operationContext()->getServiceContext());
    }

    void tearDown() override {
        _validator->shutDown();
        ConfigServerTestFixture::tearDown();
    }

    /**
     * Forces KeyManager to refresh cache and generate new keys.
     */
    void refreshKeyManager() {
        _keyManager->refreshNow(operationContext());
    }

private:
    std::unique_ptr<LogicalTimeValidator> _validator;
    std::shared_ptr<KeysCollectionManager> _keyManager;
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

TEST_F(LogicalTimeValidatorTest, CanSignTimesAfterReset) {
    validator()->enableKeyGenerator(operationContext(), true);

    LogicalTime t1(Timestamp(10, 0));
    auto newTime = validator()->trySignLogicalTime(t1);

    ASSERT_EQ(t1.asTimestamp(), newTime.getTime().asTimestamp());
    ASSERT_TRUE(newTime.getProof());

    validator()->resetKeyManagerCache();

    LogicalTime t2(Timestamp(20, 0));
    auto newTime2 = validator()->trySignLogicalTime(t2);

    ASSERT_EQ(t2.asTimestamp(), newTime2.getTime().asTimestamp());
    ASSERT_TRUE(newTime2.getProof());
}

}  // unnamed namespace
}  // namespace mongo
