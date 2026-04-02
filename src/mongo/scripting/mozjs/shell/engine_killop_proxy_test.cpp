/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/shell/engine.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo {
namespace {

/**
 * A minimal ScriptEngine implementation that counts interrupt calls for testing.
 */
class MockScriptEngine : public ScriptEngine {
public:
    void interrupt(ClientLock&, OperationContext*) override {
        interruptCount++;
    }
    void interruptAll(ServiceContextLock&) override {
        interruptAllCount++;
    }

    void runTest() override {}
    bool utf8Ok() const override {
        return true;
    }
    void enableJavaScriptProtection(bool) override {}
    bool isJavaScriptProtectionEnabled() const override {
        return false;
    }
    int getJSHeapLimitMB() const override {
        return 0;
    }
    void setJSHeapLimitMB(int) override {}
    bool getJSUseLegacyMemoryTracking() const override {
        return false;
    }
    void setJSUseLegacyMemoryTracking(bool) override {}
    std::string getLoadPath() const override {
        return "";
    }
    void setLoadPath(const std::string&) override {}
    std::string getInterpreterVersionString() const override {
        return "MockJS";
    }

    int interruptCount = 0;
    int interruptAllCount = 0;

protected:
    Scope* createScope() override {
        return nullptr;
    }
    Scope* createScopeForCurrentThread(boost::optional<int>) override {
        return nullptr;
    }
};

class ScriptEngineKillOpProxyTest : public unittest::Test,
                                    public ScopedGlobalServiceContextForTest {
protected:
    void setUp() override {
        // Register the kill-op proxy with the test's ServiceContext.
        registerScriptEngineKillOpProxy(getServiceContext());
    }

    void tearDown() override {
        // Clear the global script engine to avoid dangling state between tests.
        setGlobalScriptEngine(nullptr);
    }

    auto makeClient(std::string desc = "ScriptEngineKillOpProxyTest") {
        return getService()->makeClient(std::move(desc));
    }
};

TEST_F(ScriptEngineKillOpProxyTest, ProxyDelegatesToCurrentEngine) {
    auto* engine = new MockScriptEngine();
    setGlobalScriptEngine(engine);

    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    {
        ClientLock lk(client.get());
        getServiceContext()->killOperation(lk, opCtx.get(), ErrorCodes::Interrupted);
    }

    ASSERT_EQ(engine->interruptCount, 1);
}

TEST_F(ScriptEngineKillOpProxyTest, ProxyDelegatesToNewEngineAfterSwap) {
    auto* engine1 = new MockScriptEngine();
    setGlobalScriptEngine(engine1);

    auto client = makeClient();

    // Verify engine1 receives the interrupt.
    {
        auto opCtx = client->makeOperationContext();
        ClientLock lk(client.get());
        getServiceContext()->killOperation(lk, opCtx.get(), ErrorCodes::Interrupted);
    }
    ASSERT_EQ(engine1->interruptCount, 1);

    // Swap to engine2. This deletes engine1 — do NOT reference engine1 after this.
    auto* engine2 = new MockScriptEngine();
    setGlobalScriptEngine(engine2);

    // Create a fresh opCtx and verify engine2 receives the interrupt.
    {
        auto opCtx = client->makeOperationContext();
        ClientLock lk(client.get());
        getServiceContext()->killOperation(lk, opCtx.get(), ErrorCodes::Interrupted);
    }

    // engine2 should receive the interrupt, not engine1 (which is deleted).
    ASSERT_EQ(engine2->interruptCount, 1);
}

TEST_F(ScriptEngineKillOpProxyTest, ProxyIsNoOpWhenNoEngineSet) {
    // No engine set — proxy should silently no-op.
    auto client = makeClient();
    auto opCtx = client->makeOperationContext();

    {
        ClientLock lk(client.get());
        // Should not crash even though there's no global script engine.
        getServiceContext()->killOperation(lk, opCtx.get(), ErrorCodes::Interrupted);
    }
}

/**
 * Test fixture that exercises the real ScriptEngine::setup() code path.
 * Unlike ScriptEngineKillOpProxyTest, this does NOT manually register the proxy —
 * setup() is expected to do that itself.
 */
class ScriptEngineSetupTest : public unittest::Test, public ScopedGlobalServiceContextForTest {
protected:
    void tearDown() override {
        setGlobalScriptEngine(nullptr);
    }

    auto makeClient(std::string desc = "ScriptEngineSetupTest") {
        return getService()->makeClient(std::move(desc));
    }
};

TEST_F(ScriptEngineSetupTest, SetupCreatesGlobalEngine) {
    ASSERT_EQ(getGlobalScriptEngine(), nullptr);

    ScriptEngine::setup(ExecutionEnvironment::TestRunner);

    ASSERT_NE(getGlobalScriptEngine(), nullptr);
}

TEST_F(ScriptEngineSetupTest, SetupIsIdempotent) {
    ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    auto* firstEngine = getGlobalScriptEngine();
    ASSERT_NE(firstEngine, nullptr);

    // Calling setup again should be a no-op — same engine pointer.
    ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    ASSERT_EQ(getGlobalScriptEngine(), firstEngine);
}

TEST_F(ScriptEngineSetupTest, KillOpProxyWorksAfterSetupThenEngineSwap) {
    // Call the real setup(), which creates a MozJSScriptEngine and registers the proxy.
    ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    ASSERT_NE(getGlobalScriptEngine(), nullptr);

    auto client = makeClient();

    // Verify the proxy works with the MozJS engine (should not crash).
    {
        auto opCtx = client->makeOperationContext();
        ClientLock lk(client.get());
        getServiceContext()->killOperation(lk, opCtx.get(), ErrorCodes::Interrupted);
    }

    // Swap the engine to a MockScriptEngine. This deletes the MozJS engine.
    auto* mockEngine = new MockScriptEngine();
    setGlobalScriptEngine(mockEngine);

    // The proxy (registered by setup()) should now delegate to mockEngine.
    {
        auto opCtx = client->makeOperationContext();
        ClientLock lk(client.get());
        getServiceContext()->killOperation(lk, opCtx.get(), ErrorCodes::Interrupted);
    }

    ASSERT_EQ(mockEngine->interruptCount, 1);
}

}  // namespace
}  // namespace mongo
