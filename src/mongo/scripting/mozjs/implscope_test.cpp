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

#include "mongo/scripting/mozjs/implscope.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <js/GCAPI.h>

namespace mongo {
namespace mozjs {
namespace {

class MozJSImplScopeTest : public unittest::Test {};

// This test mimics an operation like a mapReduce over a collection scan. It invokes a JS function
// once per document, which causes it to be wrapped in a BSONHolder proxy that pins the memory until
// the proxy is finalized. SpiderMonkey does not account for that memory, so GC won't be triggered
// if not for the _notePinnedHostBytes mechanism.
TEST_F(MozJSImplScopeTest, AccumulatedPinnedHostBytesForcesGC) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    {
        std::unique_ptr<mongo::Scope> scope(
            mongo::getGlobalScriptEngine()->newScopeForCurrentThread());
        auto* implscope = dynamic_cast<mongo::mozjs::MozJSImplScope*>(scope.get());
        ASSERT_TRUE(implscope != nullptr);

        auto noopEmitHook = [](const BSONObj& args, void* data) -> BSONObj {
            return BSONObj();
        };
        scope->injectNative("emit", noopEmitHook);

        // The map function just reads a single field, but the whole document is wrapped and pinned
        // in memory by BSONHolder.
        ScriptingFunction mapFn = scope->createFunction(
            "function() { var arithmetic = this.value * 1.000001 + 3.14159;"
            " emit('all', arithmetic);}");

        // 1000 documents of ~64 KB pins ~64 MB of host BSON, which should be enough to trigger GC
        constexpr int kNumDocs = 1000;
        constexpr size_t kPaddingBytes = 64 * 1024;
        const std::string padding(kPaddingBytes, 'x');

        auto* cx = implscope->getJSContextForTest();
        auto gcsBefore = JS_GetGCParameter(cx, JSGC_NUMBER);

        const BSONObj emptyArgs;
        for (int i = 0; i < kNumDocs; ++i) {
            BSONObjBuilder builder;
            builder.append("value", static_cast<double>(i));
            builder.append("padding", padding);
            const BSONObj doc = builder.obj();

            ASSERT_EQ(
                0,
                scope->invoke(mapFn, &emptyArgs, &doc, 0 /* timeoutMs */, true /* ignoreReturn */));
        }

        auto gcsAfter = JS_GetGCParameter(cx, JSGC_NUMBER);
        ASSERT_GT(gcsAfter, gcsBefore);

        // Verify that reset() can also trigger GC.
        // Keep the pinned BSON above the 1 MB reset threshold but below the 32 MB per-invocation
        // threshold so reset() is responsible for forcing the GC.
        constexpr int kNumResetDocs = 20;
        for (int i = 0; i < kNumResetDocs; ++i) {
            BSONObjBuilder builder;
            builder.append("value", static_cast<double>(i));
            builder.append("padding", padding);
            const BSONObj doc = builder.obj();

            ASSERT_EQ(
                0,
                scope->invoke(mapFn, &emptyArgs, &doc, 0 /* timeoutMs */, true /* ignoreReturn */));
        }

        gcsBefore = JS_GetGCParameter(cx, JSGC_NUMBER);
        scope->reset();

        gcsAfter = JS_GetGCParameter(cx, JSGC_NUMBER);
        ASSERT_GT(gcsAfter, gcsBefore);
    }
    setGlobalScriptEngine(nullptr);
}

}  // namespace

}  // namespace mozjs
}  // namespace mongo
