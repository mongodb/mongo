// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/scripting/deadline_monitor.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/wasm/bridge/bridge.h"
#include "mongo/util/modules.h"
#include "mongo/util/scopeguard.h"

#include <atomic>
#include <string_view>
#include <unordered_set>

#include <boost/optional.hpp>

namespace mongo::mozjs {

class WasmtimeImplScope : public Scope {
public:
    WasmtimeImplScope(std::shared_ptr<wasm::WasmEngineContext> wasmEngineCtx,
                      boost::optional<int> jsHeapLimitMB = boost::none);

    // Constructor for reusing a pre-initialized bridge from the thread-local cache.
    // init() calls resetRealm() for cross-request isolation, which invalidates all compiled
    // function handles — cachedFunctions is accepted for API symmetry with
    // parkBridgeForCurrentThread but is always discarded. Callers must not rely on it being
    // preserved.
    WasmtimeImplScope(std::shared_ptr<wasm::WasmEngineContext> wasmEngineCtx,
                      boost::optional<int> jsHeapLimitMB,
                      std::unique_ptr<wasm::MozJSWasmBridge> idleBridge,
                      FunctionCacheMap cachedFunctions);

    ~WasmtimeImplScope() override;

    int invoke(ScriptingFunction func,
               const BSONObj* args,
               const BSONObj* recv,
               int timeoutMs = 0,
               bool ignoreReturn = false,
               bool readOnlyArgs = false,
               bool readOnlyRecv = false) override;

    void injectNative(const char* field, NativeFunction func, void* data) override;

    // --- Scope (engine.h) overrides ---
    void reset() override;
    void init(const BSONObj* data) override;
    void registerOperation(OperationContext* opCtx) override;
    void unregisterOperation() override;
    BSONObj getObject(const char* field) override;
    std::string getString(const char* field) override;
    bool getBoolean(const char* field) override;
    double getNumber(const char* field) override;
    int getNumberInt(const char* field) override;
    long long getNumberLongLong(const char* field) override;
    Decimal128 getNumberDecimal(const char* field) override;
    OID getOID(const char* field) override;
    void getBinData(const char* field,
                    std::function<void(const BSONBinData&)> withBinData) override;
    Timestamp getTimestamp(const char* field) override;
    JSRegEx getRegEx(const char* field) override;
    void setElement(const char* field, const BSONElement& e, const BSONObj& parent) override;
    void setNumber(const char* field, double val) override;
    void setString(const char* field, std::string_view val) override;
    void setObject(const char* field, const BSONObj& obj, bool readOnly = true) override;
    void setBoolean(const char* field, bool val) override;
    void setFunction(const char* field, const char* code) override;
    int type(const char* field) override;
    void rename(const char* from, const char* to) override;
    std::string getError() override;
    bool hasOutOfMemoryException() override;
    void kill() override;
    bool isKillPending() const override;

    // Used by WasmtimeScriptEngine::interrupt()/interruptAll(), which have direct, synchronous
    // access to the opCtx's own kill code at the exact moment it was set (see
    // ServiceContext::killOperation(): markKilled() and kill-op-listener notification happen in
    // the same call, on the same thread -- no race). Recording that reason here, at the source,
    // is what lets _invokeWithDeadlineMonitoring below translate Interrupted traps without
    // reconstructing the reason later from possibly-stale opCtx state.
    void killWithReason(ErrorCodes::Error reason);

    // Single-call predicate path: skips the 2 extra WASM crossings for setObject/setBoolean
    // and the per-invocation deadline monitor, since epoch interruption already covers us.
    bool execPredicate(ScriptingFunction func, const BSONObj& doc, int timeoutMs) override;

    void requireOwnedObjects() override {}

    // generation is auto-advanced by the engine at each invocation entry; no scope-level
    // call is needed (and the scope has no generation counter of its own).
    void advanceGeneration() override {}

    // The following methods are not meaningful for Wasmtime.

    bool exec(std::string_view code,
              const std::string& name,
              bool printResult,
              bool reportError,
              bool assertOnError,
              int timeoutMs = 0) override;

    // These methods are mongosh-specific and not needed as part of the server runtime.
    std::string getBaseURL() const override {
        uasserted(11605402,
                  "Calls to `getBaseURL()` are unsupported with this JS engine configuration");
    }
    void externalSetup() override {
        uasserted(11605403,
                  "Calls to `externalSetup` are unsupported with this JS engine configuration.");
    }
    void gc() override {
        uasserted(11605404, "calls to `gc()` are unsupported in this JS engine configuration.");
    }

protected:
    ScriptingFunction _createFunction(const char* code) override;

private:
    // useNewWasmRealm=true uses resetRealm() (once per request, for cross-request isolation);
    // false uses resetEngine() (per-document fast scrub). Keep private: callers that accidentally
    // pass false when true is required would silently skip cross-request isolation.
    void init(const BSONObj* data, bool useNewWasmRealm);

    // Returns the current opId as a correlation key in LOGV2 traces. Never load-bearing.
    uint64_t _currentOpId() const {
        auto* opCtx = _opCtx.load(std::memory_order_relaxed);
        return opCtx ? static_cast<uint64_t>(opCtx->getOpID()) : 0;
    }

    // Rebuilt on reset() after a kill so engine-wide state (e.g. interrupt epoch) doesn't persist
    // across the pool handoff. Owned solely by this scope.
    std::shared_ptr<wasm::WasmEngineContext> _wasmEngineCtx;
    const boost::optional<int> _jsHeapLimitMB;

    // Starts the deadline monitor, calls f(), and stops it (even on exception).
    //
    // No exception translation happens here anymore: the bridge itself throws the correct,
    // final mongo error directly from _throwAfterTrap(), using the reason recorded at the exact
    // moment kill() was called (see bridge.h's kill() doc comment and scope.cpp's
    // kill()/killWithReason()). The two callers of kill() cover every case at the source, with
    // no reconstruction needed later:
    //   - WasmtimeScriptEngine::interrupt()/interruptAll() call killWithReason() with the opCtx's
    //     own kill code, read synchronously in the same call that set it (killOp/killSessions,
    //     an explicit cursor/operation kill, client disconnect, shutdown/stepdown, or a genuinely
    //     expired maxTimeMS -- see ServiceContext::killOperation()).
    //   - This scope's own JS-fn DeadlineMonitor calls the plain kill() (defaulting to
    //     Interrupted) when internalQueryJavaScriptFnTimeoutMillis fires. That's never a
    //     client-facing maxTimeMS, so Interrupted is the correct, honest reason regardless of how
    //     much of the opCtx's own maxTimeMS remains.
    template <typename F>
    auto _invokeWithDeadlineMonitoring(int timeoutMs, F&& f) -> decltype(f()) {
        _deadlineMonitor.startDeadline(this, timeoutMs);
        ScopeGuard guard([&] { _deadlineMonitor.stopDeadline(this); });
        return f();
    }

    std::unique_ptr<wasm::MozJSWasmBridge> _bridge;
    int64_t _storeLinearMemBytes = 0;
    void _drainEmitToCallback();
    void _installHelpers();
    BSONObj _resolveGlobal(const char* field) const;
    NativeFunction _emitCallback = nullptr;
    void* _emitCallbackData = nullptr;
    std::atomic<OperationContext*> _opCtx{nullptr};  // NOLINT: explicit memory orders required
    // Declared last so ~DeadlineMonitor() joins the monitor thread before any member the poll
    // reads (notably _opCtx) is destroyed: the poll dereferences _opCtx from that thread, and
    // reverse-declaration destruction order must tear the thread down first.
    DeadlineMonitor<WasmtimeImplScope> _deadlineMonitor;

    // setupEmit allocates ~117 MB of WASM linear memory each call; cache the last byte limit
    // so we skip the WIT round-trip when it hasn't changed. Reset to 0 on bridge teardown/reset.
    int64_t _emitSetupBytes = 0;

    // Scope-local sequence number for LOGV2 correlation across scope.cpp and bridge.cpp.
    uint64_t _invokeSeq = 0;

    // Cached return value from the last invoke(); avoids a WASM round-trip in getXxx().
    BSONObj _lastReturnValue;
};

}  // namespace mongo::mozjs
