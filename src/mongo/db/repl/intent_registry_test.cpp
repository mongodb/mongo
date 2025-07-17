/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/repl/intent_guard.h"
#include "mongo/db/repl/intent_registry_test_fixture.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/thread.h"


namespace mongo {
using namespace rss::consensus;
using namespace std::chrono_literals;

// Helper function for performing a function f for each intent type, for numIterations.
auto executePerIntent = [](const std::function<void(IntentRegistry::Intent)>& f,
                           const size_t numIterations = 1) {
    for (size_t i = 0; i < numIterations; i++) {
        for (auto intent : {IntentRegistry::Intent::Write,
                            IntentRegistry::Intent::LocalWrite,
                            IntentRegistry::Intent::Read}) {
            f(intent);
        }
    }
};

#ifdef _WIN32
uint32_t kPostInterruptSleepMs = 1000;
#else
uint32_t kPostInterruptSleepMs = 100;
#endif

TEST_F(IntentRegistryTest, RegisterDeregisterIntent) {
    _intentRegistry.enable();
    std::vector<IntentRegistry::IntentToken> tokens;
    auto serviceContext = getServiceContext();
    size_t client_i = 0;
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        contexts;

    std::vector<size_t> expectedIntentCounts((size_t)IntentRegistry::Intent::_NumDistinctIntents_);
    auto createTokens = [&](IntentRegistry::Intent intent) {
        contexts.emplace_back();
        contexts.back().first =
            serviceContext->getService()->makeClient(std::to_string(client_i++));
        contexts.back().second = contexts.back().first->makeOperationContext();
        auto opCtx = contexts.back().second.get();
        auto registerResult = _intentRegistry.registerIntent(intent, opCtx);
        tokens.push_back(registerResult);
        ASSERT_TRUE(containsToken(registerResult));

        expectedIntentCounts[(size_t)intent] += 1;
        ASSERT_EQUALS(expectedIntentCounts, _intentRegistry.getTotalIntentsDeclared());
    };
    executePerIntent(createTokens, 10);
    for (auto token : tokens) {
        _intentRegistry.deregisterIntent(token);
        ASSERT_FALSE(containsToken(token));

        expectedIntentCounts[(size_t)token.intent()] -= 1;
        ASSERT_EQUALS(expectedIntentCounts, _intentRegistry.getTotalIntentsDeclared());
    }
}

TEST_F(IntentRegistryTest, DestroyingGuardDeregistersIntent) {
    _intentRegistry.enable();
    auto serviceContext = getServiceContext();
    size_t client_i = 0;
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        contexts;
    std::vector<std::unique_ptr<IntentGuard>> guards;
    std::vector<size_t> expectedIntentCounts((size_t)IntentRegistry::Intent::_NumDistinctIntents_);

    // Create and register 10 IntentGuards of each Intent type.
    auto createIntentGuards = [&](IntentRegistry::Intent intent) {
        contexts.emplace_back();
        contexts.back().first =
            serviceContext->getService()->makeClient(std::to_string(client_i++));
        contexts.back().second = contexts.back().first->makeOperationContext();
        auto opCtx = contexts.back().second.get();
        guards.emplace_back(std::make_unique<IntentGuard>(intent, opCtx));
        ASSERT_TRUE(guards.back()->intent() != boost::none);

        expectedIntentCounts[(size_t)intent] += 1;
        ASSERT_EQUALS(expectedIntentCounts, _intentRegistry.getTotalIntentsDeclared());
    };
    executePerIntent(createIntentGuards, 10);

    // Clear the guards vector, which calls the destructor and deregisters the intent.
    guards.clear();

    auto assertRegistryEmpty = [&](IntentRegistry::Intent intent) {
        ASSERT_EQUALS(0, getMapSize(intent));
    };
    executePerIntent(assertRegistryEmpty);

    std::fill(expectedIntentCounts.begin(), expectedIntentCounts.end(), 0);
    ASSERT_EQUALS(expectedIntentCounts, _intentRegistry.getTotalIntentsDeclared());
}

TEST_F(IntentRegistryTest, KillConflictingOperationsStepUp) {
    _intentRegistry.enable();
    uint32_t timeout_sec = 10;
    auto serviceContext = getServiceContext();
    size_t client_i = 0;
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        contexts;
    std::vector<std::unique_ptr<IntentGuard>> guards;

    // Create and register 10 IntentGuards of each Intent type.
    auto createIntentGuards = [&](IntentRegistry::Intent intent) {
        contexts.emplace_back();
        contexts.back().first =
            serviceContext->getService()->makeClient(std::to_string(client_i++));
        contexts.back().second = contexts.back().first->makeOperationContext();
        auto opCtx = contexts.back().second.get();
        guards.emplace_back(std::make_unique<IntentGuard>(intent, opCtx));
        ASSERT_TRUE(guards.back()->intent() != boost::none);
    };
    executePerIntent(createIntentGuards, 10);

    auto client = serviceContext->getService()->makeClient(std::to_string(client_i++));
    auto opCtx = client->makeOperationContext();

    // killConflictingOperations with a StepUp interruption should reject any attempts to register a
    // Write Intent while the interruption is ongoing.
    auto kill = _intentRegistry.killConflictingOperations(
        IntentRegistry::InterruptionType::StepUp, opCtx.get(), timeout_sec);
    stdx::this_thread::sleep_for(stdx::chrono::milliseconds(kPostInterruptSleepMs));
    kill.get();

    // Assert no operations were killed and no intents were deregistered.
    for (auto& guard : guards) {
        boost::optional<IntentRegistry::Intent> intent = guard->intent();
        if (intent != boost::none) {
            ASSERT_EQUALS(guard->getOperationContext()->getKillStatus(), ErrorCodes::OK);
        }
    }
    auto assertRegistryFull = [&](IntentRegistry::Intent intent) {
        ASSERT_EQUALS(10, getMapSize(intent));
    };
    executePerIntent(assertRegistryFull);
}

DEATH_TEST_F(IntentRegistryTest, KillConflictingOperationsDrainTimeout, "9795401") {
    // This test checks that killConflictingOperations correctly fasserts when there is an interrupt
    // and the intents are not deregistered within the drain timeout.
    _intentRegistry.enable();
    uint32_t timeout_sec = 10;
    auto serviceContext = getServiceContext();
    size_t client_i = 0;

    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        contexts;
    std::vector<std::unique_ptr<IntentGuard>> guards;

    // Create and register 10 IntentGuards of each Intent type.
    auto createIntentGuards = [&](IntentRegistry::Intent intent) {
        contexts.emplace_back();
        contexts.back().first =
            serviceContext->getService()->makeClient(std::to_string(client_i++));
        contexts.back().second = contexts.back().first->makeOperationContext();
        auto opCtx = contexts.back().second.get();
        guards.emplace_back(std::make_unique<IntentGuard>(intent, opCtx));
        ASSERT_TRUE(guards.back()->intent() != boost::none);
    };
    executePerIntent(createIntentGuards, 10);

    auto client = serviceContext->getService()->makeClient(std::to_string(client_i++));
    auto opCtx = client->makeOperationContext();
    // killConflictingOperations will timeout if there is an existing kill and the intents are not
    // deregistered within the drain timeout.
    auto kill = _intentRegistry.killConflictingOperations(
        IntentRegistry::InterruptionType::Shutdown, opCtx.get(), timeout_sec);

    kill.get();
}

DEATH_TEST_F(IntentRegistryTest, KillConflictingOperationsDrainSingleTimerTimeout, "9795401") {
    // This test checks that killConflictingOperations correctly fasserts when there is an interrupt
    // and it takes each intent to be deregistered less than a timeout period,
    // but total time it takes all three types of intents to deregister excceeds that period
    _intentRegistry.enable();
    uint32_t timeout_sec = 10;
    auto serviceContext = getServiceContext();
    size_t client_i = 0;

    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        contexts;
    std::vector<std::unique_ptr<IntentGuard>> guards;

    // Create and register 10 IntentGuards of each Intent type.
    auto createIntentGuards = [&](IntentRegistry::Intent intent) {
        contexts.emplace_back();
        contexts.back().first =
            serviceContext->getService()->makeClient(std::to_string(client_i++));
        contexts.back().second = contexts.back().first->makeOperationContext();
        auto opCtx = contexts.back().second.get();
        guards.emplace_back(std::make_unique<IntentGuard>(intent, opCtx));
        ASSERT_TRUE(guards.back()->intent() != boost::none);
    };
    executePerIntent(createIntentGuards, 1);
    // guards = {Write, LocalWrite, Read}
    // kill order: Write, Read, LocalWrite

    // killConflictingOperations will timeout if there is an existing kill and the intents are not
    // deregistered within the drain timeout.
    auto client = serviceContext->getService()->makeClient(std::to_string(client_i++));
    auto opCtx = client->makeOperationContext();
    auto kill = _intentRegistry.killConflictingOperations(
        IntentRegistry::InterruptionType::Shutdown, opCtx.get(), timeout_sec);

    // total deregister time 2.1s > 2s
    stdx::this_thread::sleep_for(5s);
    // Deregister Write
    guards[0]->reset();
    stdx::this_thread::sleep_for(4s);
    // Deregister Read
    guards[2]->reset();
    stdx::this_thread::sleep_for(1.5s);
    // Deregister LocalWrite
    guards[1]->reset();
    kill.get();
}

TEST_F(IntentRegistryTest, KillConflictingOperationsReleaseGuard) {
    // This test checks that killConflictingOperations can be called again after transition guard
    // returned by previous operation was released.
    _intentRegistry.enable();
    uint32_t timeout_sec = 10;
    auto serviceContext = getServiceContext();
    size_t client_i = 0;

    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        contexts;
    std::vector<std::unique_ptr<IntentGuard>> guards;

    // Create and register 10 IntentGuards of each Intent type.
    auto createIntentGuards = [&](IntentRegistry::Intent intent) {
        contexts.emplace_back();
        contexts.back().first =
            serviceContext->getService()->makeClient(std::to_string(client_i++));
        contexts.back().second = contexts.back().first->makeOperationContext();
        auto opCtx = contexts.back().second.get();
        guards.emplace_back(std::make_unique<IntentGuard>(intent, opCtx));
        ASSERT_TRUE(guards.back()->intent() != boost::none);
    };
    executePerIntent(createIntentGuards, 10);

    auto client = serviceContext->getService()->makeClient(std::to_string(client_i++));
    auto opCtx = client->makeOperationContext();
    auto kill = _intentRegistry.killConflictingOperations(
        IntentRegistry::InterruptionType::StepUp, opCtx.get(), timeout_sec);

    auto int_guard = kill.get();
    int_guard.release();
    kill = _intentRegistry.killConflictingOperations(
        IntentRegistry::InterruptionType::Shutdown, opCtx.get(), timeout_sec);
    guards.clear();
    kill.get();
}

TEST_F(IntentRegistryTest, KillConflictingOperationsBackToBack) {
    // This test checks that 2 consequitive calls for killConflictingOperations can be made if the
    // first finishes on time
    _intentRegistry.enable();
    uint32_t timeout_sec = 10;
    auto serviceContext = getServiceContext();
    size_t client_i = 0;

    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        contexts;
    std::vector<std::unique_ptr<IntentGuard>> guards;

    // Create and register 10 IntentGuards of each Intent type.
    auto createIntentGuards = [&](IntentRegistry::Intent intent) {
        contexts.emplace_back();
        contexts.back().first =
            serviceContext->getService()->makeClient(std::to_string(client_i++));
        contexts.back().second = contexts.back().first->makeOperationContext();
        auto opCtx = contexts.back().second.get();
        guards.emplace_back(std::make_unique<IntentGuard>(intent, opCtx));
        ASSERT_TRUE(guards.back()->intent() != boost::none);
    };
    executePerIntent(createIntentGuards, 10);

    auto client = serviceContext->getService()->makeClient(std::to_string(client_i++));
    auto opCtx = client->makeOperationContext();
    auto killsd = _intentRegistry.killConflictingOperations(
        IntentRegistry::InterruptionType::StepDown, opCtx.get(), timeout_sec);
    // Killing all writes to let stepdown kill finish in separate thread
    stdx::thread killwrites = stdx::thread([&] {
        for (auto& guard : guards) {
            if (guard->intent() == IntentRegistry::Intent::Write) {
                guard->reset();
            }
        }
        stdx::this_thread::sleep_for(1s);
        auto sdguard = killsd.get();
        sdguard.release();
    });
    // Another call for kill conflicting ops, will block till above thread finishes;
    auto killsh = _intentRegistry.killConflictingOperations(
        IntentRegistry::InterruptionType::Shutdown, opCtx.get(), timeout_sec);
    guards.clear();
    (void)killsh.get();
    killwrites.join();
}


TEST_F(IntentRegistryTest, KillConflictingOperationsDestroyGuard) {
    // This test checks that killConflictingOperations can be called again after transition guard
    // returned by previous operation was destroyed.
    _intentRegistry.enable();
    uint32_t timeout_sec = 10;
    auto serviceContext = getServiceContext();
    size_t client_i = 0;

    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        contexts;
    std::vector<std::unique_ptr<IntentGuard>> guards;

    // Create and register 10 IntentGuards of each Intent type.
    auto createIntentGuards = [&](IntentRegistry::Intent intent) {
        contexts.emplace_back();
        contexts.back().first =
            serviceContext->getService()->makeClient(std::to_string(client_i++));
        contexts.back().second = contexts.back().first->makeOperationContext();
        auto opCtx = contexts.back().second.get();
        guards.emplace_back(std::make_unique<IntentGuard>(intent, opCtx));
        ASSERT_TRUE(guards.back()->intent() != boost::none);
    };
    executePerIntent(createIntentGuards, 10);

    auto client = serviceContext->getService()->makeClient(std::to_string(client_i++));
    auto opCtx = client->makeOperationContext();
    auto kill = _intentRegistry.killConflictingOperations(
        IntentRegistry::InterruptionType::StepUp, opCtx.get(), timeout_sec);

    {
        // Get a guard and immediately destroy to enable additional interrupt
        auto int_guard = kill.get();
    }
    kill = _intentRegistry.killConflictingOperations(
        IntentRegistry::InterruptionType::Shutdown, opCtx.get(), timeout_sec);
    guards.clear();
    kill.get();
}


TEST_F(IntentRegistryTest, KillConflictingOperationsShutdown) {
    _intentRegistry.enable();
    uint32_t timeout_sec = 10;
    auto serviceContext = getServiceContext();
    size_t client_i = 0;
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        contexts;
    std::vector<std::unique_ptr<IntentGuard>> guards;

    // Create and register 10 IntentGuards of each Intent type.
    auto createIntentGuards = [&](IntentRegistry::Intent intent) {
        contexts.emplace_back();
        contexts.back().first =
            serviceContext->getService()->makeClient(std::to_string(client_i++));
        contexts.back().second = contexts.back().first->makeOperationContext();
        auto opCtx = contexts.back().second.get();
        guards.emplace_back(std::make_unique<IntentGuard>(intent, opCtx));
        ASSERT_TRUE(guards.back()->intent() != boost::none);
    };
    executePerIntent(createIntentGuards, 10);

    // killConflictingOperations with a Shutdown interruption should kill all operations
    // registered.
    auto client = serviceContext->getService()->makeClient(std::to_string(client_i++));
    auto opCtx = client->makeOperationContext();
    auto kill = _intentRegistry.killConflictingOperations(
        IntentRegistry::InterruptionType::Shutdown, opCtx.get(), timeout_sec);

    stdx::this_thread::sleep_for(stdx::chrono::milliseconds(kPostInterruptSleepMs));

    // Any attempt to register an intent during a Shutdown interruption should throw an
    // exception.
    auto newClient = serviceContext->getService()->makeClient("testClient");
    auto newOpCtx = newClient->makeOperationContext();
    try {
        WriteIntentGuard writeGuard(newOpCtx.get());
        // Fail the test if constructing the intentGuard succeeds, as it means it incorrectly
        // registered the intent and did not throw an exception.
        ASSERT_TRUE(false);
    } catch (const DBException&) {
        // Fails as expected, continue.
    }
    try {
        IntentGuard localWriteGuard(IntentRegistry::Intent::LocalWrite, newOpCtx.get());
        ASSERT_TRUE(false);
    } catch (const DBException&) {
        // Fails as expected, continue.
    }
    try {
        IntentGuard readGuard(IntentRegistry::Intent::Read, newOpCtx.get());
        ASSERT_TRUE(false);
    } catch (const DBException&) {
        // Fails as expected, continue.
    }

    // Assert the registries are the same size since no new intents are allowed and none of the
    // killed operations have deregistered their intents yet.
    auto assertRegistryFull = [&](IntentRegistry::Intent intent) {
        ASSERT_EQUALS(10, getMapSize(intent));
    };
    executePerIntent(assertRegistryFull);


    // Confirm each operation was killed and deregister each guard.
    for (auto& guard : guards) {
        boost::optional<IntentRegistry::Intent> intent = guard->intent();
        if (intent != boost::none) {
            ASSERT_EQUALS(guard->getOperationContext()->getKillStatus(),
                          ErrorCodes::InterruptedAtShutdown);
            guard->reset();
        }
    }

    auto assertRegistryEmpty = [&](IntentRegistry::Intent intent) {
        ASSERT_EQUALS(0, getMapSize(intent));
    };
    executePerIntent(assertRegistryEmpty);

    kill.get();
}

TEST_F(IntentRegistryTest, KillConflictingOperationsSameOpCtxCanDeclareIntents) {
    _intentRegistry.enable();
    uint32_t timeout_sec = 10;
    auto serviceContext = getServiceContext();
    size_t client_i = 0;
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        contexts;
    std::vector<std::unique_ptr<IntentGuard>> guards;

    // Create and register 10 IntentGuards of each Intent type.
    auto createIntentGuards = [&](IntentRegistry::Intent intent) {
        contexts.emplace_back();
        contexts.back().first =
            serviceContext->getService()->makeClient(std::to_string(client_i++));
        contexts.back().second = contexts.back().first->makeOperationContext();
        auto opCtx = contexts.back().second.get();
        guards.emplace_back(std::make_unique<IntentGuard>(intent, opCtx));
        ASSERT_TRUE(guards.back()->intent() != boost::none);
    };
    executePerIntent(createIntentGuards, 10);

    // killConflictingOperations with a Shutdown interruption should kill all operations
    // registered.
    auto client = serviceContext->getService()->makeClient(std::to_string(client_i++));
    auto opCtx = client->makeOperationContext();
    auto kill = _intentRegistry.killConflictingOperations(
        IntentRegistry::InterruptionType::Shutdown, opCtx.get(), timeout_sec);

    stdx::this_thread::sleep_for(stdx::chrono::milliseconds(kPostInterruptSleepMs));

    // Since we are using the same opCtx we should be able to register any intent.
    {
        WriteIntentGuard writeGuard(opCtx.get());
    }

    {
        IntentGuard localWriteGuard(IntentRegistry::Intent::LocalWrite, opCtx.get());
    }

    {
        IntentGuard readGuard(IntentRegistry::Intent::Read, opCtx.get());
    }

    // Assert the registries are the same size since no new intents are allowed and none of the
    // killed operations have deregistered their intents yet.
    auto assertRegistryFull = [&](IntentRegistry::Intent intent) {
        ASSERT_EQUALS(10, getMapSize(intent));
    };
    executePerIntent(assertRegistryFull);

    // Confirm each operation was killed and deregister each guard.
    for (auto& guard : guards) {
        if (guard->intent()) {
            ASSERT_EQUALS(guard->getOperationContext()->getKillStatus(),
                          ErrorCodes::InterruptedAtShutdown);
            guard->reset();
        }
    }

    auto assertRegistryEmpty = [&](IntentRegistry::Intent intent) {
        ASSERT_EQUALS(0, getMapSize(intent));
    };
    executePerIntent(assertRegistryEmpty);

    kill.get();
}


TEST_F(IntentRegistryTest, KillConflictingOperationsRollback) {
    _intentRegistry.enable();
    uint32_t timeout_sec = 10;
    auto serviceContext = getServiceContext();
    size_t client_i = 0;
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        contexts;
    std::vector<std::unique_ptr<IntentGuard>> guards;

    // Create and register 10 IntentGuards of each Intent type.
    auto createIntentGuards = [&](IntentRegistry::Intent intent) {
        contexts.emplace_back();
        contexts.back().first =
            serviceContext->getService()->makeClient(std::to_string(client_i++));
        contexts.back().second = contexts.back().first->makeOperationContext();
        auto opCtx = contexts.back().second.get();
        guards.emplace_back(std::make_unique<IntentGuard>(intent, opCtx));
        ASSERT_TRUE(guards.back()->intent() != boost::none);
    };
    executePerIntent(createIntentGuards, 10);

    // killConflictingOperations with a Rollback interruption should kill all operations
    // registered.
    auto client = serviceContext->getService()->makeClient(std::to_string(client_i++));
    auto opCtx = client->makeOperationContext();
    auto kill = _intentRegistry.killConflictingOperations(
        IntentRegistry::InterruptionType::Rollback, opCtx.get(), timeout_sec);
    stdx::this_thread::sleep_for(stdx::chrono::milliseconds(kPostInterruptSleepMs));

    // Any attempt to register an intent during a Rollback interruption should throw an
    // exception.
    auto newClient = serviceContext->getService()->makeClient("testClient");
    auto newOpCtx = newClient->makeOperationContext();
    try {
        WriteIntentGuard writeGuard(newOpCtx.get());
        // Fail the test if constructing the intentGuard succeeds, as it means it incorrectly
        // registered the intent and did not throw an exception.
        ASSERT_TRUE(false);
    } catch (const DBException&) {
        // Fails as expected, continue.
    }

    {
        // Local Write intent can succeed during rollback.
        IntentGuard localWriteGuard(IntentRegistry::Intent::LocalWrite, newOpCtx.get());
    }

    try {
        IntentGuard readGuard(IntentRegistry::Intent::Read, newOpCtx.get());
        ASSERT_TRUE(false);
    } catch (const DBException&) {
        // Fails as expected, continue.
    }

    // Assert the registries are the same size since no new intents are allowed and none of the
    // killed operations have deregistered their intents yet.
    auto assertRegistryFull = [&](IntentRegistry::Intent intent) {
        ASSERT_EQUALS(10, getMapSize(intent));
    };
    executePerIntent(assertRegistryFull, 1);

    // Confirm each operation was killed and deregister each guard.
    for (auto& guard : guards) {
        boost::optional<IntentRegistry::Intent> intent = guard->intent();
        if (intent != boost::none && intent != IntentRegistry::Intent::LocalWrite) {
            ASSERT_EQUALS(guard->getOperationContext()->getKillStatus(),
                          ErrorCodes::InterruptedDueToReplStateChange);
            // Deregister the Write Intents.
            guard->reset();
        } else {
            if (intent) {
                // LocalWrite uneffected by Rollback not killed.
                ASSERT_EQUALS(guard->getOperationContext()->getKillStatus(), ErrorCodes::OK);
            }
        }
    }

    auto assertRegistrySize = [&](IntentRegistry::Intent intent) {
        ASSERT_EQUALS((intent == IntentRegistry::Intent::LocalWrite ? 10 : 0), getMapSize(intent));
    };
    executePerIntent(assertRegistrySize);
    kill.get();
}

TEST_F(IntentRegistryTest, KillConflictingOperationsStepDown) {
    auto serviceContext = getServiceContext();
    _intentRegistry.enable();
    uint32_t timeout_sec = 10;
    size_t client_i = 0;
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        contexts;
    std::vector<std::unique_ptr<IntentGuard>> guards;

    // Create and register 10 IntentGuards of each Intent type.
    auto createIntentGuards = [&](IntentRegistry::Intent intent) {
        contexts.emplace_back();
        contexts.back().first =
            serviceContext->getService()->makeClient(std::to_string(client_i++));
        contexts.back().second = contexts.back().first->makeOperationContext();
        auto opCtx = contexts.back().second.get();
        guards.emplace_back(std::make_unique<IntentGuard>(intent, opCtx));
        ASSERT_TRUE(guards.back()->intent() != boost::none);
    };
    executePerIntent(createIntentGuards, 10);

    // killConflictingOperations with a StepDown interruption should kill all operations
    // registered with Write Intent, and will reject any attempts to register a Write Intent
    // while the interruption is ongoing.
    auto client = serviceContext->getService()->makeClient(std::to_string(client_i++));
    auto opCtx = client->makeOperationContext();
    auto kill = _intentRegistry.killConflictingOperations(
        IntentRegistry::InterruptionType::StepDown, opCtx.get(), timeout_sec);
    stdx::this_thread::sleep_for(stdx::chrono::milliseconds(kPostInterruptSleepMs));
    {
        auto clientWritePrepared =
            serviceContext->getService()->makeClient("testClientWritePrepared");
        auto opCtx = clientWritePrepared->makeOperationContext();

        try {
            WriteIntentGuard writeGuard(opCtx.get());
            // Fail the test if constructing the intentGuard succeeds, as it means it incorrectly
            // registered the intent and did not throw an exception.
            ASSERT_TRUE(false);
        } catch (const DBException&) {
            // Fails as expected, continue.
        }

        // Read and Local Write intents are compatible with ongoing StepDown interruption.
        auto clientRead = serviceContext->getService()->makeClient("testClientRead");
        auto opCtxRead = clientRead->makeOperationContext();
        IntentGuard readGuard(IntentRegistry::Intent::Read, opCtxRead.get());
        ASSERT_TRUE(readGuard.intent() != boost::none);

        auto clientLocalWrite = serviceContext->getService()->makeClient("testClientLocalWrite");
        auto opCtxLocalWrite = clientLocalWrite->makeOperationContext();
        IntentGuard localWriteGuard(IntentRegistry::Intent::LocalWrite, opCtxLocalWrite.get());
        ASSERT_TRUE(localWriteGuard.intent() != boost::none);

        ASSERT_EQUALS(10, getMapSize(IntentRegistry::Intent::Write));
        ASSERT_EQUALS(11, getMapSize(IntentRegistry::Intent::Read));
        ASSERT_EQUALS(11, getMapSize(IntentRegistry::Intent::LocalWrite));
    }

    for (auto& guard : guards) {
        if (guard->intent() == boost::none) {
            continue;
        }
        auto intent = guard->intent();
        // All operations with Write Intent were interrupted.
        if (intent == IntentRegistry::Intent::Write) {
            ASSERT_EQUALS(guard->getOperationContext()->getKillStatus(),
                          ErrorCodes::InterruptedDueToReplStateChange);
            // Deregister the Write Intents.
            guard->reset();

        } else {
            // All other intents are unaffected by StepDown.
            ASSERT_EQUALS(guard->getOperationContext()->getKillStatus(), ErrorCodes::OK);
        }
    }
    // The extra read and local write guards are no longer within scope, so destroying it also
    // deregistered it, reducing the Read and LocalWrite registry sizes back to the
    // initial 10 respectively.
    auto assertRegistrySize = [&](IntentRegistry::Intent intent) {
        ASSERT_EQUALS(((intent == IntentRegistry::Intent::Write) ? 0 : 10), getMapSize(intent));
    };
    executePerIntent(assertRegistrySize);
    kill.get();
}

TEST_F(IntentRegistryTest, IntegrityRegistryEnableDisable) {
    auto serviceContext = getServiceContext();
    _intentRegistry.disable();
    auto createGuardDuringDisable = [&](IntentRegistry::Intent intent) {
        auto client = serviceContext->getService()->makeClient("testClient");
        auto opCtx = client->makeOperationContext();
        try {
            // IntentGuard constructor will throw an exception when IntentRegistry is disabled.
            IntentGuard guard(intent, opCtx.get());
            // Fail the test if constructing the intentGuard succeeds, as it means it did not throw
            // an exception.
            ASSERT_TRUE(false);
        } catch (const DBException&) {
            // Fails as expected, continue.
        }
    };
    executePerIntent(createGuardDuringDisable);

    _intentRegistry.enable();
    auto createGuardDuringEnable = [&](IntentRegistry::Intent intent) {
        auto client = serviceContext->getService()->makeClient("testClient");
        auto opCtx = client->makeOperationContext();
        IntentGuard guard(intent, opCtx.get());
        ASSERT_TRUE(guard.intent() != boost::none);  // IntentRegistry enabled.
    };
    executePerIntent(createGuardDuringEnable);

    // Disable the intent registry again and assert that any attempt to register will be rejected.
    _intentRegistry.disable();
    executePerIntent(createGuardDuringDisable);
}


}  // namespace mongo
