// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
