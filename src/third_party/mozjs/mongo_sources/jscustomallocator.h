/*    Copyright 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <cstdlib>
#include <cstring>

#include <jstypes.h>

#include "jscustomallocator_oom.h"

namespace mongo {
namespace sm {
JS_PUBLIC_API size_t get_total_bytes();
JS_PUBLIC_API void reset(size_t max_bytes);
JS_PUBLIC_API size_t get_max_bytes();
}  // namespace sm
}  // namespace mongo

typedef size_t arena_id_t;

JS_PUBLIC_API void* js_malloc(size_t bytes);
JS_PUBLIC_API void* js_calloc(size_t bytes);
JS_PUBLIC_API void* js_calloc(size_t nmemb, size_t size);
JS_PUBLIC_API void js_free(void* p);
JS_PUBLIC_API void* js_realloc(void* p, size_t bytes);
JS_PUBLIC_API void* js_arena_malloc(arena_id_t arena, size_t bytes);
JS_PUBLIC_API void* js_arena_calloc(arena_id_t arena, size_t bytes);
JS_PUBLIC_API void* js_arena_calloc(arena_id_t arena, size_t nmemb, size_t size);
JS_PUBLIC_API void* js_arena_realloc(arena_id_t arena, void* p, size_t bytes);


// Malloc allocation.

namespace js {

extern JS_PUBLIC_DATA arena_id_t MallocArena;
extern JS_PUBLIC_DATA arena_id_t ArrayBufferContentsArena;
extern JS_PUBLIC_DATA arena_id_t StringBufferArena;

extern void InitMallocAllocator();
extern void ShutDownMallocAllocator();

// This is a no-op if built without MOZ_MEMORY and MOZ_DEBUG.
extern void AssertJSStringBufferInCorrectArena(const void* ptr);

} /* namespace js */

namespace js {

/* Disable OOM testing in sections which are not OOM safe. */
struct MOZ_RAII JS_PUBLIC_DATA AutoEnterOOMUnsafeRegion {
    MOZ_NORETURN MOZ_COLD void crash(const char* reason);
    MOZ_NORETURN MOZ_COLD void crash(size_t size, const char* reason);

    using AnnotateOOMAllocationSizeCallback = void (*)(size_t);
    static mozilla::Atomic<AnnotateOOMAllocationSizeCallback, mozilla::Relaxed>
        annotateOOMSizeCallback;
    static void setAnnotateOOMAllocationSizeCallback(AnnotateOOMAllocationSizeCallback callback) {
        annotateOOMSizeCallback = callback;
    }

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    AutoEnterOOMUnsafeRegion() : oomEnabled_(oom::simulator.isThreadSimulatingAny()) {
        if (oomEnabled_) {
            MOZ_ALWAYS_TRUE(owner_.compareExchange(nullptr, this));
            oom::simulator.setInUnsafeRegion(true);
        }
    }

    ~AutoEnterOOMUnsafeRegion() {
        if (oomEnabled_) {
            oom::simulator.setInUnsafeRegion(false);
            MOZ_ALWAYS_TRUE(owner_.compareExchange(this, nullptr));
        }
    }

private:
    // Used to catch concurrent use from other threads.
    static mozilla::Atomic<AutoEnterOOMUnsafeRegion*> owner_;

    bool oomEnabled_;
#endif
};

} /* namespace js */
