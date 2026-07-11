// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

    // Non-zero when the error originated from a DBException (e.g. uassert).
    // Holds the ErrorCodes::Error value so the bridge can rethrow with the
    // original code rather than always mapping to JSInterpreterFailure.
    uint32_t mongo_error_code;
} wasm_mozjs_error_t;

}  // namespace wasm
}  // namespace mozjs
}  // namespace mongo

