/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/operation_key_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ServiceContextClientTest : public unittest::Test, public ScopedGlobalServiceContextForTest {
protected:
    class CountingClientObserver : public ServiceContext::ClientObserver {
    public:
        void onCreateClient(Client* client) override {
            createClientCount++;
        }
        void onDestroyClient(Client* client) override {
            destroyClientCount++;
        }
        void onCreateOperationContext(OperationContext* opCtx) override {
            createOpCtxCount++;
        }
        void onDestroyOperationContext(OperationContext* opCtx) override {
            destroyOpCtxCount++;
        }

        int createClientCount = 0;
        int destroyClientCount = 0;
        int createOpCtxCount = 0;
        int destroyOpCtxCount = 0;
    };

    void setUp() override {
        auto observerUniq = std::make_unique<CountingClientObserver>();
        countingClientObserver = observerUniq.get();
        getServiceContext()->registerClientObserver(std::move(observerUniq));
    }

    auto makeClient(std::string desc = "ServiceContextTest") {
        return getService()->makeClient(std::move(desc));
    }

    CountingClientObserver* countingClientObserver = nullptr;
};

TEST_F(ServiceContextClientTest, MakeClient) {
    auto client = makeClient();
    ASSERT_NE(client, nullptr);

    ASSERT_EQ(countingClientObserver->createClientCount, 1);

    ServiceContext::LockedClientsCursor cursor(getGlobalServiceContext());
    ASSERT_EQ(cursor.next(), client.get());
    ASSERT_EQ(cursor.next(), nullptr);
}

TEST_F(ServiceContextClientTest, ClientObserverNoDestroyIfOnCreateThrows) {
    class ThrowingClientObserver : public CountingClientObserver {
        void onCreateClient(Client* client) override {
            CountingClientObserver::onCreateClient(client);
            uasserted(ErrorCodes::InternalError, "throwing for test");
        }
    };

    auto observerUniq = std::make_unique<ThrowingClientObserver>();
    auto observer = observerUniq.get();
    getServiceContext()->registerClientObserver(std::move(observerUniq));

    ASSERT_THROWS_CODE(makeClient(), DBException, ErrorCodes::InternalError);

    // Observer should not be notified of the destruction.
    ASSERT_EQ(observer->createClientCount, 1);
    ASSERT_EQ(observer->destroyClientCount, 0);

    // No client should've been added to the clients list.
    ServiceContext::LockedClientsCursor cursor(getGlobalServiceContext());
    ASSERT_EQ(cursor.next(), nullptr);
}

TEST_F(ServiceContextClientTest, DeleteClient) {
    {
        auto client = makeClient();
        ASSERT_NE(client, nullptr);
    }

    ASSERT_EQ(countingClientObserver->destroyClientCount, 1);

    {
        ServiceContext::LockedClientsCursor cursor(getGlobalServiceContext());
        ASSERT_EQ(cursor.next(), nullptr);
    }
}

TEST_F(ServiceContextClientTest, MakeAndDeleteClientWithOperationIdManager) {
    OperationId opId;

    {
        auto client = makeClient();
        auto opCtx = client->makeOperationContext();
        opId = opCtx->getOpID();
        auto clientLock = OperationIdManager::get(getServiceContext()).findAndLockClient(opId);
        ASSERT_TRUE(clientLock);
    }

    auto clientLock = OperationIdManager::get(getServiceContext()).findAndLockClient(opId);
    ASSERT_FALSE(clientLock);
}

class ServiceContextOpContextTest : public ServiceContextClientTest {
protected:
    class CountingKillOpListener : public KillOpListenerInterface {
    public:
        void interrupt(ClientLock&, OperationContext*) override {
            interruptCount++;
        }
        void interruptAll(ServiceContextLock&) override {
            interruptAllCount++;
        }
        int interruptCount = 0;
        int interruptAllCount = 0;
    };

    void setUp() override {
        ServiceContextClientTest::setUp();
        getServiceContext()->registerKillOpListener(&countingKillOpListener);
    }

    CountingKillOpListener countingKillOpListener;
};

TEST_F(ServiceContextOpContextTest, MakeOperationContext) {
    class CreateOpCtxObserver : public CountingClientObserver {
        void onCreateOperationContext(OperationContext* opCtx) override {
            CountingClientObserver::onCreateOperationContext(opCtx);
            // By the time the observer sees the OpCtx, it should have a baton.
            ASSERT_NE(opCtx->getBaton(), nullptr);
        }
    };

    auto observerUniq = std::make_unique<CreateOpCtxObserver>();
    auto observer = observerUniq.get();
    getServiceContext()->registerClientObserver(std::move(observerUniq));

    auto client = makeClient();
    auto opCtx = client->makeOperationContext();
    ASSERT_NE(opCtx, nullptr);

    ASSERT_EQ(observer->createOpCtxCount, 1);

    // The Client and OpCtx should be linked to each other.
    ASSERT_EQ(client->getOperationContext(), opCtx.get());
    ASSERT_EQ(opCtx->getClient(), client.get());
}

TEST_F(ServiceContextOpContextTest, MakeOperationContextCreatesOperationId) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    // Creating an opCtx should create an OperationId that is observable in the
    // OperationIdManager.
    auto clientLock =
        OperationIdManager::get(getServiceContext()).findAndLockClient(opCtx->getOpID());
    ASSERT_TRUE(clientLock);
}

DEATH_TEST_F(ServiceContextOpContextTest,
             MakeOperationContextFailsWhenAlreadyExists,
             "Tripwire assertion") {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    // This will trip a tripwire assert and kill the previous opCtx.
    ASSERT_THROWS(client->makeOperationContext(), DBException);
    ASSERT_NOT_OK(opCtx->getKillStatus());
}

TEST_F(ServiceContextOpContextTest, DeleteOperationContext) {
    auto client = makeClient();

    // We'll schedule a task on the baton that will overwrite this with the status it runs with.
    // We'll expect to see a ShutdownInProgress status, indicating the baton was detached.
    Status batonTaskStatus = Status::OK();

    {
        auto opCtx = client->makeOperationContext();

        // Schedule a task on the baton so we can observe it detaching.
        auto baton = opCtx->getBaton();
        invariant(baton);
        baton->schedule([&](Status status) { batonTaskStatus = status; });
    }

    ASSERT_EQ(countingClientObserver->destroyOpCtxCount, 1);
    ASSERT_EQ(client->getOperationContext(), nullptr);
    ASSERT_EQ(batonTaskStatus.code(), ErrorCodes::ShutdownInProgress);
}

TEST_F(ServiceContextOpContextTest, DelistOperation) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    getServiceContext()->delistOperation(opCtx.get());

    ASSERT_EQ(client->getOperationContext(), nullptr);
    ASSERT_EQ(countingKillOpListener.interruptAllCount, 0);
    ASSERT_EQ(countingKillOpListener.interruptCount, 0);
}

TEST_F(ServiceContextOpContextTest, DelistOperationWithOperationKey) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    // No operation key by default.
    ASSERT_EQ(opCtx->getOperationKey(), boost::none);

    auto opKey = UUID::gen();
    opCtx->setOperationKey(opKey);
    ASSERT_EQ(opCtx->getOperationKey(), opKey);

    // Operation key is tracked in OperationKeyManager.
    ASSERT_NE(OperationKeyManager::get(client.get()).at(opKey), boost::none);

    // Delisting the operation releases the OperationKey from the manager.
    getServiceContext()->delistOperation(opCtx.get());
    ASSERT_EQ(OperationKeyManager::get(client.get()).at(opKey), boost::none);
}

DEATH_TEST_F(ServiceContextOpContextTest, DelistOperationWrongServiceContext, "Invariant failure") {
    auto otherServiceContext = ServiceContext::make();
    auto otherClient = otherServiceContext->getService()->makeClient("other client");
    auto opCtx = otherClient->makeOperationContext();

    getServiceContext()->delistOperation(opCtx.get());
}

TEST_F(ServiceContextOpContextTest, KillOperation) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    {
        ClientLock lk(client.get());
        getServiceContext()->killOperation(lk, opCtx.get(), ErrorCodes::InternalError);
    }

    ASSERT_THROWS_CODE(opCtx->checkForInterrupt(), DBException, ErrorCodes::InternalError);
    ASSERT_EQUALS(opCtx->getKillStatus(), ErrorCodes::InternalError);
    ASSERT_EQ(countingKillOpListener.interruptAllCount, 0);
    ASSERT_EQ(countingKillOpListener.interruptCount, 1);
}

TEST_F(ServiceContextOpContextTest, KillAndDelistOperation) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    getServiceContext()->killAndDelistOperation(opCtx.get(), ErrorCodes::InternalError);

    ASSERT_EQ(client->getOperationContext(), nullptr);
    ASSERT_EQUALS(opCtx->getKillStatus(), ErrorCodes::InternalError);
    ASSERT_EQ(countingKillOpListener.interruptAllCount, 0);
    ASSERT_EQ(countingKillOpListener.interruptCount, 1);
}

TEST_F(ServiceContextOpContextTest, SetKillAllOperationsNewOpCtxAreInterrupted) {
    auto client = makeClient();

    getServiceContext()->setKillAllOperations();

    auto opCtx = client->makeOperationContext();
    ASSERT_THROWS_CODE(opCtx->checkForInterrupt(), DBException, ErrorCodes::InterruptedAtShutdown);
    ASSERT_OK(opCtx->getKillStatus());
    ASSERT_EQ(countingKillOpListener.interruptCount, 0);
    ASSERT_EQ(countingKillOpListener.interruptAllCount, 1);
}

TEST_F(ServiceContextOpContextTest, SetKillAllOperationsActiveOpCtxAreInterruptedAndKilled) {
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    getServiceContext()->setKillAllOperations();

    ASSERT_THROWS_CODE(opCtx->checkForInterrupt(), DBException, ErrorCodes::InterruptedAtShutdown);
    ASSERT_EQUALS(opCtx->getKillStatus(), ErrorCodes::InterruptedAtShutdown);
}

TEST_F(ServiceContextOpContextTest, SetKillAllOperationsExcludedClients) {
    auto clientNotExcluded = makeClient("NotExcluded");
    auto clientExcluded = makeClient("Excluded");

    {
        auto opCtxNotExcluded = clientNotExcluded->makeOperationContext();
        auto opCtxExcluded = clientExcluded->makeOperationContext();

        getServiceContext()->setKillAllOperations({"Excluded"});

        ASSERT_EQUALS(opCtxNotExcluded->getKillStatus(), ErrorCodes::InterruptedAtShutdown);
        ASSERT_THROWS_CODE(
            opCtxNotExcluded->checkForInterrupt(), DBException, ErrorCodes::InterruptedAtShutdown);

        // The excluded client is not killed, but it is still interrupted.
        ASSERT_OK(opCtxExcluded->getKillStatus());
        ASSERT_THROWS_CODE(
            opCtxExcluded->checkForInterrupt(), DBException, ErrorCodes::InterruptedAtShutdown);
    }

    ASSERT_EQ(countingKillOpListener.interruptCount, 1);
    ASSERT_EQ(countingKillOpListener.interruptAllCount, 1);

    // New opCtxs are immediately interrupted but not killed, even for the excluded clients.
    auto opCtxExcluded = clientExcluded->makeOperationContext();
    ASSERT_THROWS_CODE(
        opCtxExcluded->checkForInterrupt(), DBException, ErrorCodes::InterruptedAtShutdown);
    ASSERT_OK(opCtxExcluded->getKillStatus());

    auto opCtxNotExcluded = clientNotExcluded->makeOperationContext();
    ASSERT_THROWS_CODE(
        opCtxNotExcluded->checkForInterrupt(), DBException, ErrorCodes::InterruptedAtShutdown);
    ASSERT_OK(opCtxNotExcluded->getKillStatus());

    ASSERT_EQ(countingKillOpListener.interruptCount, 1);
    ASSERT_EQ(countingKillOpListener.interruptAllCount, 1);
}

DEATH_TEST_F(ServiceContextOpContextTest,
             DeleteServiceContextInvariantsWithActiveClients,
             "Invariant failure") {
    auto client = makeClient();
    setGlobalServiceContext({});
}

class ConstructorDestructorActionListener {
public:
    void onConstruct(std::string event) {
        constructEvents.push_back(std::move(event));
    }
    void onDestruct(std::string event) {
        destructEvents.push_back(std::move(event));
    }
    void reset() {
        constructEvents.clear();
        destructEvents.clear();
    }
    std::vector<std::string> constructEvents, destructEvents;
};

ConstructorDestructorActionListener actionListener;

template <typename T>
typename T::ConstructorActionRegisterer registerConstructorAction(
    std::string name, std::vector<std::string> prereqs = {}) {
    return typename T::ConstructorActionRegisterer{name,
                                                   prereqs,
                                                   [name](T*) { actionListener.onConstruct(name); },
                                                   [name](T*) {
                                                       actionListener.onDestruct(name);
                                                   }};
}

const auto serviceContext1Registerer = registerConstructorAction<ServiceContext>("ServiceContext1");
const auto serviceContext2Registerer =
    registerConstructorAction<ServiceContext>("ServiceContext2", {"ServiceContext1"});
const auto serviceContext3Registerer = registerConstructorAction<ServiceContext>(
    "ServiceContext3", {"ServiceContext1", "ServiceContext2"});
const auto service1Registerer = registerConstructorAction<Service>("Service1");
const auto service2Registerer = registerConstructorAction<Service>("Service2", {"Service1"});
const auto service3Registerer =
    registerConstructorAction<Service>("Service3", {"Service1", "Service2"});

TEST(ConstructorDestructorActionTest, RegisteredConstructorActionsRunInOrder) {
    actionListener.reset();
    ServiceContext::make();

    // Service ConstructorActions run before ServiceContext ConstructorActions.
    std::vector<std::string> expected{"Service1",
                                      "Service2",
                                      "Service3",
                                      "ServiceContext1",
                                      "ServiceContext2",
                                      "ServiceContext3"};
    ASSERT_EQ(actionListener.constructEvents, expected);

    std::reverse(expected.begin(), expected.end());
    ASSERT_EQ(actionListener.destructEvents, expected);
}

}  // namespace
}  // namespace mongo
