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

#include <cstdio>
#include <cstdlib>
#include <exception>

// Boost source_location stub
namespace boost {
struct source_location {
    const char* file_name() const {
        return "";
    }
    const char* function_name() const {
        return "";
    }
    unsigned int line() const {
        return 0;
    }
    unsigned int column() const {
        return 0;
    }
};
}  // namespace boost

extern "C" {

// Exception allocation
void* __cxa_allocate_exception(size_t thrown_size) {
    (void)thrown_size;
    std::fprintf(stderr,
                 "FATAL: __cxa_allocate_exception called - exceptions not supported in WASM\n");
    __builtin_trap();
    return nullptr;
}

void __cxa_free_exception(void* thrown_exception) {
    (void)thrown_exception;
    std::fprintf(stderr, "FATAL: __cxa_free_exception called - exceptions not supported in WASM\n");
    __builtin_trap();
}

// Throwing
void __cxa_throw(void* thrown_exception, void* tinfo, void (*dest)(void*)) {
    (void)thrown_exception;
    (void)tinfo;
    (void)dest;
    std::fprintf(stderr, "FATAL: __cxa_throw called - exceptions not supported in WASM\n");
    __builtin_trap();
}

void __cxa_rethrow(void) {
    std::fprintf(stderr, "FATAL: __cxa_rethrow called - exceptions not supported in WASM\n");
    __builtin_trap();
}

// Catching
void* __cxa_begin_catch(void* exception_object) {
    (void)exception_object;
    std::fprintf(stderr, "FATAL: __cxa_begin_catch called - exceptions not supported in WASM\n");
    __builtin_trap();
    return nullptr;
}

void __cxa_end_catch(void) {
    std::fprintf(stderr, "FATAL: __cxa_end_catch called - exceptions not supported in WASM\n");
    __builtin_trap();
}

// WASM-specific exception handling
struct __wasm_lpad_context_t {
    int selector;
    void* exception;
};
struct __wasm_lpad_context_t __wasm_lpad_context = {0, nullptr};

int _Unwind_CallPersonality(void* exception_object) {
    (void)exception_object;
    std::fprintf(stderr,
                 "FATAL: _Unwind_CallPersonality called - exceptions not supported in WASM\n");
    __builtin_trap();
    return 0;
}

// Additional exception-related functions
void* __cxa_get_exception_ptr(void* exception_object) {
    (void)exception_object;
    return nullptr;
}

void __cxa_pure_virtual(void) {
    std::fprintf(stderr, "FATAL: pure virtual function called\n");
    __builtin_trap();
}

void __cxa_deleted_virtual(void) {
    std::fprintf(stderr, "FATAL: deleted virtual function called\n");
    __builtin_trap();
}

}  // extern "C"

// Boost exception stubs
namespace boost {

void throw_exception(std::exception const& e) {
    std::fprintf(stderr, "FATAL: boost::throw_exception called: %s\n", e.what());
    __builtin_trap();
}

void throw_exception(std::exception const& e, boost::source_location const& loc) {
    std::fprintf(stderr,
                 "FATAL: boost::throw_exception called at %s:%u: %s\n",
                 loc.file_name(),
                 loc.line(),
                 e.what());
    __builtin_trap();
}

}  // namespace boost
