// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/shared/mozjs_error_types.h"

#include <cstdio>
#include <cstring>

#include "jsapi.h"
#include "utils.h"


namespace mongo {
namespace mozjs {
namespace wasm {

inline void clear_error(wasm_mozjs_error_t* e) {
    if (!e)
        return;

    if (e->msg) {
        cabi_realloc(e->msg, e->msg_len + 1, 1, 0);
        e->msg = nullptr;
        e->msg_len = 0;
    }
    if (e->filename) {
        cabi_realloc(e->filename, e->filename_len + 1, 1, 0);
        e->filename = nullptr;
        e->filename_len = 0;
    }
    if (e->stack) {
        cabi_realloc(e->stack, e->stack_len + 1, 1, 0);
        e->stack = nullptr;
        e->stack_len = 0;
    }

    e->code = SM_OK;
    e->line = 0;
    e->column = 0;
    e->mongo_error_code = 0;
}

// Execution check helper for error handling
class ExecutionCheck {
public:
    ExecutionCheck(JSContext* cx, wasm_mozjs_error_t* out_err) : _cx(cx), _out(out_err) {
        clear_error(out_err);
    }

    bool ok(bool success, err_code_t onFail = SM_E_JSAPI_FAIL) {
        if (success)
            return true;
        capture(onFail);
        return false;
    }

    template <class T>
    T* okPtr(T* p, err_code_t onFail = SM_E_JSAPI_FAIL) {
        if (p)
            return p;
        capture(onFail);
        return nullptr;
    }

private:
    void capture(err_code_t fallback);

    JSContext* _cx;
    wasm_mozjs_error_t* _out;
};


}  // namespace wasm

// WASI-compatible error report to string (declaration).
// Implementation in error.cpp.
JSString* mongoErrorReportToString(JSContext* cx, JSErrorReport* reportp);

}  // namespace mozjs
}  // namespace mongo
