// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/scripting/config_engine_gen.h"
#include "mongo/scripting/config_engine_wasm_gen.h"
#include "mongo/scripting/mozjs/wasm/scope/scope.h"
#include "mongo/scripting/mozjs/wasm/scope/scope_test_fixture.h"
#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

using namespace mongo;
using namespace mongo::mozjs;

// --- wasmtimeStoreMemoryLimitMB server parameter ---

// The default server parameter value (256 MB) allows normal scope creation and execution.
TEST(WasmtimeScope, MemoryLimit_DefaultValueWorks) {
    // Don't override — use whatever the IDL default is.
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction fn = scope->createFunction("return 1 + 2;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(3.0, scope->getNumber("__returnValue"));
}

// Changing the parameter at runtime affects newly created scopes.
TEST(WasmtimeScope, MemoryLimit_RuntimeChangeAffectsNewScope) {
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    auto savedHeap = gJSHeapLimitMB.load();
    // heap=200, overhead=max(64,20)=64, minStore=264.  store=512 satisfies.
    gJSHeapLimitMB.store(200);
    gWasmtimeStoreMemoryLimitMB.store(512);
    ON_BLOCK_EXIT([&] {
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
        gJSHeapLimitMB.store(savedHeap);
    });

    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction fn = scope->createFunction("return 'hello';");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(std::string("hello"), scope->getString("__returnValue"));
}

// After reset(), the scope re-reads the store memory limit from the server parameter.
// Note: the JS heap limit is cached per-scope at construction time, so only the store
// limit changes on reset.
TEST(WasmtimeScope, MemoryLimit_ResetPicksUpNewValue) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    // Change the store limit after the scope was created, then reset.
    // Use store=1500 which differs from the default and satisfies the constraint.
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    gWasmtimeStoreMemoryLimitMB.store(1500);
    ON_BLOCK_EXIT([&] { gWasmtimeStoreMemoryLimitMB.store(savedStore); });
    scope->reset();

    // The scope should still work — init() re-reads the parameter.
    ScriptingFunction fn = scope->createFunction("return 99;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(99.0, scope->getNumber("__returnValue"));
}

// --- jsHeapLimitMB vs wasmtimeStoreMemoryLimitMB ---

// jsHeapLimitMB must never exceed wasmtimeStoreMemoryLimitMB (the JS heap lives
// inside WASM linear memory, so the store must be large enough for the heap plus
// SpiderMonkey runtime overhead).  The bridge constructor enforces this.

// The IDL defaults (jsHeapLimitMB=1100, wasmtimeStoreMemoryLimitMB=1210) must
// satisfy the constraint: store >= heap + max(64, heap/10) = 1100 + 110 = 1210.
TEST(WasmtimeScope, MemoryLimit_DefaultIDLValuesSatisfyConstraint) {
    // Verify the raw default values encode the expected relationship.
    ASSERT_EQ(1100, gJSHeapLimitMB.load());
    ASSERT_EQ(1210, gWasmtimeStoreMemoryLimitMB.load());

    uint32_t heap = static_cast<uint32_t>(gJSHeapLimitMB.load());
    uint32_t minStore = heap + std::max(64u, heap / 10);
    ASSERT_GTE(static_cast<uint32_t>(gWasmtimeStoreMemoryLimitMB.load()), minStore);
}

// Setting jsHeapLimitMB equal to wasmtimeStoreMemoryLimitMB leaves no room for
// overhead, so scope creation must fail.
TEST(WasmtimeScope, MemoryLimit_HeapEqualToStoreLimitFails) {
    auto savedHeap = gJSHeapLimitMB.load();
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    ON_BLOCK_EXIT([&] {
        gJSHeapLimitMB.store(savedHeap);
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
    });

    // heap=256, store=256.  overhead=max(64,25)=64, minStore=320 > 256.
    gJSHeapLimitMB.store(256);
    gWasmtimeStoreMemoryLimitMB.store(256);

    WasmtimeScriptEngine engine;
    ASSERT_THROWS_CODE(
        engine.createScopeForCurrentThread(boost::none), DBException, ErrorCodes::BadValue);
}

// Setting jsHeapLimitMB above wasmtimeStoreMemoryLimitMB must fail.
TEST(WasmtimeScope, MemoryLimit_HeapAboveStoreLimitFails) {
    auto savedHeap = gJSHeapLimitMB.load();
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    ON_BLOCK_EXIT([&] {
        gJSHeapLimitMB.store(savedHeap);
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
    });

    gJSHeapLimitMB.store(512);
    gWasmtimeStoreMemoryLimitMB.store(256);

    WasmtimeScriptEngine engine;
    ASSERT_THROWS_CODE(
        engine.createScopeForCurrentThread(boost::none), DBException, ErrorCodes::BadValue);
}

// A store limit that satisfies heap + overhead succeeds.
TEST(WasmtimeScope, MemoryLimit_StoreExceedsHeapPlusOverheadSucceeds) {
    auto savedHeap = gJSHeapLimitMB.load();
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    ON_BLOCK_EXIT([&] {
        gJSHeapLimitMB.store(savedHeap);
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
    });

    // heap=200, overhead=max(64,20)=64, minStore=264.  store=264 is exactly enough.
    gJSHeapLimitMB.store(200);
    gWasmtimeStoreMemoryLimitMB.store(264);

    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction fn = scope->createFunction("return 42;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(42.0, scope->getNumber("__returnValue"));
}

// For large heap values the overhead switches from the 64 MB floor to 10%.
TEST(WasmtimeScope, MemoryLimit_LargeHeapUsesPercentOverhead) {
    auto savedHeap = gJSHeapLimitMB.load();
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    ON_BLOCK_EXIT([&] {
        gJSHeapLimitMB.store(savedHeap);
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
    });

    // heap=1000, overhead=max(64,100)=100, minStore=1100.
    // store=1099 is one short → must fail.
    gJSHeapLimitMB.store(1000);
    gWasmtimeStoreMemoryLimitMB.store(1099);

    WasmtimeScriptEngine engine;
    ASSERT_THROWS_CODE(
        engine.createScopeForCurrentThread(boost::none), DBException, ErrorCodes::BadValue);
}

// reset() re-reads the server params; if the new combination is invalid it must fail.
TEST(WasmtimeScope, MemoryLimit_ResetWithInvalidParamsFails) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    auto savedHeap = gJSHeapLimitMB.load();
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    ON_BLOCK_EXIT([&] {
        gJSHeapLimitMB.store(savedHeap);
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
    });

    // Make the combination invalid after the scope already exists, then reset.
    gJSHeapLimitMB.store(512);
    gWasmtimeStoreMemoryLimitMB.store(256);

    ASSERT_THROWS_CODE(scope->reset(), DBException, ErrorCodes::BadValue);
}

// Emit buffer must fit within the WASM store's linear memory.
TEST(WasmtimeScope, EmitBufferExceedsStoreLimitFails) {
    auto savedHeap = gJSHeapLimitMB.load();
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    auto savedEmit = internalQueryMaxJsEmitBytes.load();
    ON_BLOCK_EXIT([&] {
        gJSHeapLimitMB.store(savedHeap);
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
        internalQueryMaxJsEmitBytes.store(savedEmit);
    });

    // heap=64 MB, overhead=max(64, 6)=64 MB → min store=128 MB for scope init to pass.
    // With store=128 MB, emitBuf=128MB+16MB=144MB > 128MB → injectNative throws BadValue.
    gJSHeapLimitMB.store(64);
    gWasmtimeStoreMemoryLimitMB.store(128);
    internalQueryMaxJsEmitBytes.store(128 * 1024 * 1024);

    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    // The size validation in injectNative only fires on a real setup call (data != nullptr).
    // Pass a non-null dummy pointer to trigger the setupEmit path and verify the BadValue.
    int dummy = 0;
    ASSERT_THROWS_CODE(scope->injectNative(
                           "emit", [](const BSONObj&, void*) { return BSONObj(); }, &dummy),
                       DBException,
                       ErrorCodes::BadValue);
}

// ---------------------------------------------------------------------------
// OOM trap classification
// ---------------------------------------------------------------------------

// When the WASM store limiter is triggered (SpiderMonkey MOZ_CRASHes inside
// WASM linear memory), scope->invoke() must surface ExceededMemoryLimit rather
// than the opaque generic trap error code.
//
// Disabled under TSan: the MOZ_CRASH signal handler performs allocations that
// TSan intercepts before Wasmtime can convert the SIGABRT into a trap.
#if !__has_feature(thread_sanitizer)
TEST(WasmtimeScope, MemoryLimit_StoreLimiterTrapClassifiedAsExceededMemoryLimit) {
    auto savedHeap = gJSHeapLimitMB.load();
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    ON_BLOCK_EXIT([&] {
        gJSHeapLimitMB.store(savedHeap);
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
    });

    // heap=50 MB, overhead=max(64,5)=64 MB → minStore=114 MB exactly.
    gJSHeapLimitMB.store(50);
    gWasmtimeStoreMemoryLimitMB.store(114);

    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction fn = scope->createFunction(
        "function() {"
        "  var big = [];"
        "  for (var i = 0; i < 2000000; i++) {"
        "    big.push({x: i, y: i * 2, z: 'padding_string_to_consume_memory'});"
        "  }"
        "  return big.length;"
        "}");
    ASSERT_THROWS_CODE(
        scope->invoke(fn, nullptr, nullptr, 0), DBException, ErrorCodes::ExceededMemoryLimit);
}
#endif  // !__has_feature(thread_sanitizer)
