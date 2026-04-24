/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/scripting/engine.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <memory>
#include <string>

#include <jscustomallocator.h>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <fmt/format.h>

namespace mongo {
namespace mozjs {


class JSCustomAllocatorTest : public unittest::Test {
protected:
    void setUp() override {
        mongo::sm::reset(0, false);

        // Note: get_total_bytes() is an estimate and won't exactly count allocated bytes,
        // test should only compare to 0
        ASSERT_EQUALS(mongo::sm::get_total_bytes(), 0);
    }

    void tearDown() override {
        mongo::sm::reset(0, false);
    }
};


TEST_F(JSCustomAllocatorTest, MallocUpToLimit) {
    mongo::sm::reset(100, false);
    ASSERT_EQUALS(mongo::sm::get_total_bytes(), 0);

    void* ptr1 = js_malloc(20);
    ASSERT_NOT_EQUALS(ptr1, nullptr);
    ASSERT_GREATER_THAN(mongo::sm::get_total_bytes(), 0);

    void* ptr2 = js_malloc(60);
    ASSERT_NOT_EQUALS(ptr2, nullptr);

    ASSERT_EQUALS(js_malloc(50), nullptr);

    js_free(ptr2);
    ptr2 = nullptr;

    void* ptr3 = js_malloc(50);
    ASSERT_NOT_EQUALS(ptr3, nullptr);

    js_free(ptr1);
    js_free(ptr3);
    ASSERT_EQUALS(mongo::sm::get_total_bytes(), 0);
}

TEST_F(JSCustomAllocatorTest, ReallocUpToLimit) {
    mongo::sm::reset(100, false);
    ASSERT_EQUALS(mongo::sm::get_total_bytes(), 0);

    void* ptr1 = js_malloc(20);
    ASSERT_NOT_EQUALS(ptr1, nullptr);
    ASSERT_GREATER_THAN(mongo::sm::get_total_bytes(), 0);

    void* ptr2 = js_realloc(ptr1, 40);
    ASSERT_NOT_EQUALS(ptr2, nullptr);

    void* ptr3 = js_realloc(ptr2, 200);
    ASSERT_EQUALS(ptr3, nullptr);

    js_free(ptr2);
    ASSERT_EQUALS(mongo::sm::get_total_bytes(), 0);
}


TEST_F(JSCustomAllocatorTest, CallocUpToLimit) {
    mongo::sm::reset(100, false);
    ASSERT_EQUALS(mongo::sm::get_total_bytes(), 0);

    void* ptr1 = js_calloc(10, 2);
    ASSERT_NOT_EQUALS(ptr1, nullptr);
    ASSERT_GREATER_THAN(mongo::sm::get_total_bytes(), 0);

    void* ptr2 = js_calloc(10, 6);
    ASSERT_NOT_EQUALS(ptr2, nullptr);

    ASSERT_EQUALS(js_calloc(50), nullptr);

    js_free(ptr2);
    ptr2 = nullptr;

    void* ptr3 = js_calloc(50);
    ASSERT_NOT_EQUALS(ptr3, nullptr);

    js_free(ptr1);
    js_free(ptr3);
    ASSERT_EQUALS(mongo::sm::get_total_bytes(), 0);
}


// control for whether allocations are happening in a query or just during setup/teardown
TEST_F(JSCustomAllocatorTest, SetupScriptEngine) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    std::unique_ptr<mongo::Scope> scope(mongo::getGlobalScriptEngine()->newScope());
    scope.reset();
    setGlobalScriptEngine(nullptr);
}

// SERVER-113893 trigger js_arena_malloc and verify that entire buffer is valid
TEST_F(JSCustomAllocatorTest, SingleAlloc) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    std::unique_ptr<mongo::Scope> scope(mongo::getGlobalScriptEngine()->newScope());

    std::string codeStr = R"(
            const buf = new ArrayBuffer(128);
            const view = new Uint8Array(buf);
            for (let i = 0; i < 128; i++) view[i] = i;
        )";
    StringData code(codeStr);

    ASSERT_DOES_NOT_THROW(scope->exec(code,
                                      "root_module",
                                      true /* printResult */,
                                      true /* reportError */,
                                      true /* assertOnError , timeout*/));
    scope.reset();
    setGlobalScriptEngine(nullptr);
}

TEST_F(JSCustomAllocatorTest, ResizeMany) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    std::unique_ptr<mongo::Scope> scope(mongo::getGlobalScriptEngine()->newScope());

    std::string codeStr = R"(
            let buffers = [];
            const numBuffers = 100;
            for (let i = 0; i < numBuffers; i++) {
                const bufferSize = i * i;

                const buf = new ArrayBuffer(bufferSize);
                buffers.push(buf);

                // access entire buffer to make sure memory is valid
                const view = new Uint8Array(buf);
                for (let j = 0; j < bufferSize; j++) {
                    view[j] = j;
                }
            }

            // resize in reverse order so half get bigger and half get smaller
            for (let i = numBuffers - 1; i <= 0; i--) {
                const bufferSize = (numBuffers - i) * (numBuffers - i) + 3;

                buffers[i].resize(bufferSize);

                // access entire buffer to make sure memory is valid
                const view = new Uint8Array(buf);
                for (let j = 0; j < bufferSize; j++) {
                    view[j] = j;
                }
            }
        )";
    StringData code(codeStr);

    ASSERT_DOES_NOT_THROW(scope->exec(code,
                                      "root_module",
                                      true /* printResult */,
                                      true /* reportError */,
                                      true /* assertOnError , timeout*/));
    scope.reset();
    setGlobalScriptEngine(nullptr);
}

#ifdef MONGO_CONFIG_DEBUG_BUILD
// Sweep an OOM injection point across scope construction and assert that
// every failure surfaces as a DBException rather than crashing the process.
TEST_F(JSCustomAllocatorTest, OOMDuringScopeInitDoesNotCrash) {
    constexpr int64_t kDisableOomInjection = -1;

    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    ON_BLOCK_EXIT([] {
        mongo::sm::set_fail_on_allocation(kDisableOomInjection);
        setGlobalScriptEngine(nullptr);
    });

    // Chosen to comfortably exceed the allocations a full scope construction
    // performs on current SpiderMonkey; beyond this the hook never fires.
    constexpr int64_t kMaxScopeAllocationsWithMargin = 3800;
    // Step by 5 to keep the test's runtime tolerable. Set kStep to 1 when
    // upgrading SpiderMonkey so every allocation index is exercised — a new
    // crash site could hide at any single-allocation offset.
    constexpr int64_t kStep = 5;
    for (int64_t n = 0; n <= kMaxScopeAllocationsWithMargin; n += kStep) {
        mongo::sm::set_fail_on_allocation(n);
        try {
            std::unique_ptr<mongo::Scope> scope(
                mongo::getGlobalScriptEngine()->newScopeForCurrentThread());
            // Disarm so teardown allocations aren't hijacked.
            mongo::sm::set_fail_on_allocation(kDisableOomInjection);
            scope.reset();
        } catch (const DBException&) {
            mongo::sm::set_fail_on_allocation(kDisableOomInjection);
        }
    }
}
#endif  // MONGO_CONFIG_DEBUG_BUILD

}  // namespace mozjs
}  // namespace mongo
