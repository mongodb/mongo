// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"

namespace mongo::mozjs {

// Installs a WasmtimeScriptEngine as the process-global script engine for the
// lifetime of the guard and tears it down on destruction. Tests that exercise paths
// reaching for getGlobalScriptEngine() (e.g. idle-bridge reuse) need a global engine
// installed.
struct GlobalEngineGuard {
    GlobalEngineGuard() {
        setGlobalScriptEngine(new WasmtimeScriptEngine());
    }
    ~GlobalEngineGuard() {
        setGlobalScriptEngine(nullptr);
    }
    WasmtimeScriptEngine& engine() {
        return *static_cast<WasmtimeScriptEngine*>(getGlobalScriptEngine());
    }
};

}  // namespace mongo::mozjs
