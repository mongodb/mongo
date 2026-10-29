// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/operation_id.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <vector>

namespace mongo {
namespace {

class OpIdPoolTest : public ServiceContextTest {};

TEST_F(OpIdPoolTest, CanIssueId) {
    OperationIdManager& manager = OperationIdManager::get(getServiceContext());
    auto client = getClient();

    ASSERT_EQ(manager.issueForClient(client), 0);
}

TEST_F(OpIdPoolTest, IssueForTwoClients) {
    OperationIdManager& manager = OperationIdManager::get(getServiceContext());
    auto client = getClient();

    ASSERT_EQ(manager.issueForClient(client), 0);

    auto client2 = getService()->makeClient("test");
    ASSERT_EQ(manager.issueForClient(client2.get()), OperationIdManager::kDefaultLeaseSize);
}

TEST_F(OpIdPoolTest, RenewsWhenExhaustedLease) {
    OperationIdManager& manager = OperationIdManager::get(getServiceContext());
    auto client = getClient();

    for (size_t i = 0; i < OperationIdManager::kDefaultLeaseSize; i++) {
        ASSERT_EQ(manager.issueForClient(client), i);
    }

    // Add another client with a new lease in, so that the first client's call to issueId after
    // renewing its lease will only succeed if it is truly issuing from the new lease (and not
    // succeeding because _usedIds + 1 = leaseSize).
    auto client2 = getService()->makeClient("test");
    ASSERT_EQ(manager.issueForClient(client2.get()), OperationIdManager::kDefaultLeaseSize);

    ASSERT_EQ(manager.issueForClient(client), OperationIdManager::kDefaultLeaseSize * 2);
}

TEST_F(OpIdPoolTest, LeasePoolLoopsBackAround) {
    OperationIdManager& manager = OperationIdManager::get(getServiceContext());

    // Set a very large lease size so that we quickly run out of leases.
    const size_t maxNumClients = 8;
    const size_t leaseSize = (std::numeric_limits<OperationId>::max() / maxNumClients) + 1;
    manager.setLeaseSize_forTest(leaseSize);

    // Exhaust initial leases.
    for (size_t i = 0; i < maxNumClients - 1; i++) {
        auto tempClient = getService()->makeClient("test");
        ASSERT_EQ(manager.issueForClient(tempClient.get()), i * leaseSize);
    }

    // When we renew the leases, they should pop off the _released queue, which has been populated
    // by deleted client's leases.
    auto tempClient = getService()->makeClient("test");
    ASSERT_EQ(manager.issueForClient(tempClient.get()), 0);
}

using OpIdPoolTestDeathTest = OpIdPoolTest;
DEATH_TEST_F(OpIdPoolTestDeathTest, ExhaustAvailableLeases, "invariant") {
    OperationIdManager& manager = OperationIdManager::get(getServiceContext());

    // Set a very large lease size so that we quickly run out of leases.
    const size_t maxNumClients = 8;
    const size_t leaseSize = (std::numeric_limits<OperationId>::max() / maxNumClients) + 1;
    manager.setLeaseSize_forTest(leaseSize);

    std::vector<ServiceContext::UniqueClient> clients;

    for (size_t i = 0; i <= maxNumClients; i++) {
        auto client = getService()->makeClient("test" + std::to_string(i));
        ASSERT_EQ(manager.issueForClient(client.get()), i * leaseSize);
        // Keep the client alive so that the lease is not released.
        clients.push_back(std::move(client));
    }

    // We have gone up to the maximum number of clients, and so this call will fail for a new
    // client.
    auto client = getService()->makeClient("test");
    manager.issueForClient(client.get());
}

/** ---------------------- _clientByOperationId map tests ---------------------
 * All tests below must have clients make an opCtx in order to successfully call findAndLockClient.
 */
TEST_F(OpIdPoolTest, OpIdMapIsCorrect) {
    OperationIdManager& manager = OperationIdManager::get(getServiceContext());

    auto client = getService()->makeClient("test");
    auto opCtx = getServiceContext()->makeOperationContext(client.get());
    auto opId1 = client->getOperationContext()->getOpID();

    auto client2 = getService()->makeClient("test");
    auto opCtx2 = getServiceContext()->makeOperationContext(client2.get());
    auto opId2 = client2->getOperationContext()->getOpID();

    {
        auto clientFromMap1 = manager.findAndLockClient(opId1);
        ASSERT(clientFromMap1);
        ASSERT_EQ(client->getUUID(), clientFromMap1->getUUID());
    }
    {
        auto clientFromMap2 = manager.findAndLockClient(opId2);
        ASSERT(clientFromMap2);
        ASSERT_EQ(client2->getUUID(), clientFromMap2->getUUID());
    }
}

TEST_F(OpIdPoolTest, OpIdMapCorrectlyErasesClients) {
    OperationIdManager& manager = OperationIdManager::get(getServiceContext());

    OperationId opId1;
    {
        auto client1 = getService()->makeClient("test");
        auto opCtx = getServiceContext()->makeOperationContext(client1.get());
        opId1 = client1->getOperationContext()->getOpID();
    }

    auto client2 = getService()->makeClient("test");
    auto opCtx2 = getServiceContext()->makeOperationContext(client2.get());
    auto opId2 = client2->getOperationContext()->getOpID();

    ASSERT_FALSE(manager.findAndLockClient(opId1));

    auto clientFromMap2 = manager.findAndLockClient(opId2);
    ASSERT(clientFromMap2);
    ASSERT_EQ(client2->getUUID(), clientFromMap2->getUUID());
}

TEST_F(OpIdPoolTest, OpIdMapHoldsCorrectValueForEveryId) {
    OperationIdManager& manager = OperationIdManager::get(getServiceContext());
    auto client = getClient();

    // Iterate through every id and make sure that it is correctly mapped to client.
    for (size_t i = 0; i < OperationIdManager::kDefaultLeaseSize; i++) {
        auto opCtx = getServiceContext()->makeOperationContext(client);
        ASSERT_EQ(opCtx->getOpID(), i);
        {
            auto clientFromMap = manager.findAndLockClient(i);
            ASSERT(clientFromMap);
            ASSERT_EQ(client->getUUID(), clientFromMap->getUUID());
        }
    }

    // Add another client with a new lease in, so that the first client's call to issueId after
    // renewing its lease will only succeed if it is truly issuing from the new lease (and not
    // succeeding because _usedIds + 1 = leaseSize).
    auto client2 = getService()->makeClient("test");
    auto opCtx2 = getServiceContext()->makeOperationContext(client2.get());
    ASSERT_EQ(client2->getOperationContext()->getOpID(), OperationIdManager::kDefaultLeaseSize);

    // This new lease's ids are correctly mapped to the client.
    auto opCtx = getServiceContext()->makeOperationContext(client);
    auto opIdFromNewLease = opCtx->getOpID();
    ASSERT_EQ(opIdFromNewLease, OperationIdManager::kDefaultLeaseSize * 2);
    auto clientFromMap = manager.findAndLockClient(opIdFromNewLease);
    ASSERT(clientFromMap);
    ASSERT_EQ(client->getUUID(), clientFromMap->getUUID());
}

TEST_F(OpIdPoolTest, RenewLeaseErasesOldLeaseFromMap) {
    OperationIdManager& manager = OperationIdManager::get(getServiceContext());
    auto client = getService()->makeClient("test");
    auto opCtx = getServiceContext()->makeOperationContext(client.get());
    auto opId1 = client->getOperationContext()->getOpID();

    // Force the client to renew its lease by exhausting the pool of ids.
    for (size_t i = 1; i < OperationIdManager::kDefaultLeaseSize + 1; i++) {
        ASSERT_EQ(manager.issueForClient(client.get()), i);
    }

    // The outdated OperationId is now invalid in the map and does not map to the client.
    auto clientFromMap = manager.findAndLockClient(opId1);
    ASSERT_FALSE(clientFromMap);

    getServiceContext()->markOperationAsPendingDestruction(opCtx.get());
    opCtx.reset();

    // A newly issued OperationId correctly maps to the client.
    auto opCtx2 = getServiceContext()->makeOperationContext(client.get());
    auto opId2 = client->getOperationContext()->getOpID();
    auto clientFromMap2 = manager.findAndLockClient(opId2);
    ASSERT(clientFromMap2);
    ASSERT_EQ(client->getUUID(), clientFromMap2->getUUID());
}

TEST_F(OpIdPoolTest, ClientOpIdMustMatchOperationContextOpId) {
    OperationIdManager& manager = OperationIdManager::get(getServiceContext());
    auto client = getService()->makeClient("test");
    auto opCtx = getServiceContext()->makeOperationContext(client.get());
    auto opId1 = client->getOperationContext()->getOpID();

    // A newly issued OperationId that does not match the id of the opCtx on the client will not map
    // to the client.
    auto opId2 = manager.issueForClient(client.get());
    ASSERT_NOT_EQUALS(opId1, opId2);
    auto clientFromMap2 = manager.findAndLockClient(opId2);
    ASSERT_FALSE(clientFromMap2);
}

}  // namespace
}  // namespace mongo
