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
