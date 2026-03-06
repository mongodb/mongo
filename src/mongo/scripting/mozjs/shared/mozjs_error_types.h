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

#include <cstddef>
#include <cstdint>

namespace mongo {
namespace mozjs {
namespace wasm {
/**
 * Shared error codes for MozJS operations.
 * Used by both the WASM engine and the mongo host side.
 */
typedef enum {
    SM_OK = 0,

    // Wrapper / host-side errors
    SM_E_INVALID_ARG,
    SM_E_BAD_STATE,
    SM_E_NOMEM,
    SM_E_IO,
    SM_E_TIMEOUT,
    SM_E_NOT_SUPPORTED,
    SM_E_INTERNAL,

    // MozJS API usage
    SM_E_JSAPI_FAIL,
    SM_E_PENDING_EXCEPTION,
    SM_E_NO_EXCEPTION,
    SM_E_TERMINATED,
    SM_E_OOM,

    // Script failures
    SM_E_COMPILE,
    SM_E_RUNTIME,
    SM_E_MODULE,
    SM_E_PROMISE_REJECTION,
    SM_E_STACK_OVERFLOW,

    // Data marshalling / boundary errors
    SM_E_TYPE,
    SM_E_ENCODING,
} err_code_t;

typedef struct {
    char* msg;       // Error message
    size_t msg_len;  // Length of msg (excluding null terminator)

    char* filename;       // Source filename
    size_t filename_len;  // Length of filename (excluding null terminator)

    char* stack;       // Stack trace
    size_t stack_len;  // Length of stack (excluding null terminator)

    uint32_t line;
    uint32_t column;
    err_code_t code;
} wasm_mozjs_error_t;

}  // namespace wasm
}  // namespace mozjs
}  // namespace mongo

