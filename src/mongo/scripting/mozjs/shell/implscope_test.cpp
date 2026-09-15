// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/shell/implscope.h"

#include "mongo/bson/oid.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/types/oid.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

#include <boost/smart_ptr/shared_ptr.hpp>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {
namespace {

class MozJSImplScopeTest : public unittest::Test {};

std::string gCapturedJSStack;

BSONObj captureJSStackHook(const BSONObj& args, void* data) {
    auto* implscope = static_cast<mongo::mozjs::MozJSImplScope*>(data);
    gCapturedJSStack = implscope->buildStackString();
    return BSONObj();
}

// captureOOMLocation() formats "<file>:<line>" into a fixed buffer, so a long enough script name
// pushes the line number off the end. Truncating silently would be worse than useless: the reader
// would see "<file>:100" and have no way to know the real line was 1000. Drive it from a native
// hook so the capture happens with a live scripted frame, exactly as it does at an OOM.
BSONObj captureOOMLocationHook(const BSONObj& args, void* data) {
    static_cast<mongo::mozjs::MozJSImplScope*>(data)->captureOOMLocation(true /* overwrite */);
    return BSONObj();
}

TEST_F(MozJSImplScopeTest, OOMLocationIsMarkedWhenTruncated) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    {
        std::unique_ptr<mongo::Scope> scope(
            mongo::getGlobalScriptEngine()->newScopeForCurrentThread());
        auto* implscope = dynamic_cast<mongo::mozjs::MozJSImplScope*>(scope.get());
        ASSERT_TRUE(implscope != nullptr);
        scope->injectNative("__captureOOMLocation", captureOOMLocationHook, implscope);

        // A short name leaves the line number intact, so nothing should be marked.
        scope->exec("__captureOOMLocation();",
                    "shortName.js",
                    false /* printResult */,
                    true /* reportError */,
                    true /* assertOnError */);
        const auto untruncated = implscope->getOOMLocation();
        ASSERT_FALSE(untruncated.empty());
        ASSERT_TRUE(untruncated.ends_with(":1")) << untruncated;
        ASSERT_FALSE(untruncated.ends_with("...")) << untruncated;

        // A name longer than the buffer must be reported as incomplete rather than as a plausible
        // but wrong "<file>:<line>".
        const std::string longName(2 * MozJSImplScope::kMaxOOMLocationSize, 'x');
        scope->exec("__captureOOMLocation();",
                    longName,
                    false /* printResult */,
                    true /* reportError */,
                    true /* assertOnError */);
        const auto truncated = implscope->getOOMLocation();
        ASSERT_EQ(truncated.size(), MozJSImplScope::kMaxOOMLocationSize - 1);
        ASSERT_TRUE(truncated.ends_with("...")) << truncated;
    }
    setGlobalScriptEngine(nullptr);
}

// The out-of-memory abort must key off the engine's own signal, not the error message. Matching
// the message let ordinary JavaScript reach the abort with `throw new Error("out of memory")`,
// which would dump core on a script the user wrote. hasOutOfMemoryException() is only set by
// setOOM(), from the allocator and SpiderMonkey's callback, so JavaScript cannot forge it.
TEST_F(MozJSImplScopeTest, JsThrownOutOfMemoryMessageIsNotTreatedAsRealOOM) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    {
        std::unique_ptr<mongo::Scope> scope(
            mongo::getGlobalScriptEngine()->newScopeForCurrentThread());
        auto* implscope = dynamic_cast<mongo::mozjs::MozJSImplScope*>(scope.get());
        ASSERT_TRUE(implscope != nullptr);

        ASSERT_THROWS(scope->exec("throw new Error('out of memory');",
                                  "jsThrownOOM",
                                  false /* printResult */,
                                  true /* reportError */,
                                  true /* assertOnError */),
                      DBException);

        // The message says "out of memory", but the engine never reported one.
        ASSERT_FALSE(implscope->hasOutOfMemoryException());
    }
    setGlobalScriptEngine(nullptr);
}

TEST_F(MozJSImplScopeTest, BuildStackStringResolvesJSFramesDuringExecution) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    {
        std::unique_ptr<mongo::Scope> scope(
            mongo::getGlobalScriptEngine()->newScopeForCurrentThread());
        auto* implscope = dynamic_cast<mongo::mozjs::MozJSImplScope*>(scope.get());
        ASSERT_TRUE(implscope != nullptr);

        gCapturedJSStack.clear();
        scope->injectNative("__captureJSStack", captureJSStackHook, implscope);
        scope->exec(
            "function aDistinctlyNamedFrame() { __captureJSStack(); }"
            "aDistinctlyNamedFrame();",
            "buildStackStringTest",
            false /* printResult */,
            true /* reportError */,
            true /* assertOnError */);

        // The captured stack must name the JS function that was on the stack; an empty or
        // frameless string would make the OOM report no more useful than it is today.
        ASSERT_FALSE(gCapturedJSStack.empty());
        ASSERT_STRING_CONTAINS(gCapturedJSStack, "aDistinctlyNamedFrame");
    }
    setGlobalScriptEngine(nullptr);
}

TEST_F(MozJSImplScopeTest, JsExceptionToStatusOutOfMemoryCheck) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    {
        std::unique_ptr<mongo::Scope> scope(
            mongo::getGlobalScriptEngine()->newScopeForCurrentThread());
        auto* implscope = dynamic_cast<mongo::mozjs::MozJSImplScope*>(scope.get());
        ASSERT_TRUE(implscope != nullptr);
        JSAutoRealm ac(implscope->getJSContextForTest(), implscope->getGlobalForTest());
        JS_ReportOutOfMemory(implscope->getJSContextForTest());
        auto status = currentJSExceptionToStatus(implscope->getJSContextForTest(),
                                                 ErrorCodes::InternalError,
                                                 "Triggered exception from unit test.");
        ASSERT_EQ(status.code(), ErrorCodes::JSInterpreterFailure);
        ASSERT_STRING_CONTAINS(status.reason(), ErrorMessage::kOutOfMemory);
    }
    setGlobalScriptEngine(nullptr);
}

// OIDInfo::getOID returns a nil OID unless the object both stores an OID in its reserved slot
// AND has the class passed as checkClass. This test isolates the class check: it builds a fully
// constructed ObjectId (so its reserved slot is populated and the GetMaybePtrFromReservedSlot
// path can't be the reason for a nil result) and then calls getOID with a non-OID class. The
// only thing that can make getOID return OID() here is the checkClass mismatch.
TEST_F(MozJSImplScopeTest, GetOIDReturnsNilOIDOnClassMismatch) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    {
        std::unique_ptr<mongo::Scope> scope(
            mongo::getGlobalScriptEngine()->newScopeForCurrentThread());
        auto* implscope = dynamic_cast<mongo::mozjs::MozJSImplScope*>(scope.get());
        ASSERT_TRUE(implscope != nullptr);

        auto* cx = implscope->getJSContextForTest();
        JSAutoRealm ac(cx, implscope->getGlobalForTest());

        auto* runtime = getCommonRuntime(cx);
        const JSClass* oidClass = runtime->oidProto().getJSClass();
        const JSClass* nonOidClass = runtime->numberLongProto().getJSClass();

        // Build a real ObjectId so its reserved slot holds a live OID pointer.
        const OID expected("507f1f77bcf86cd799439011");
        JS::RootedValue oidVal(cx);
        OIDInfo::make(cx, expected, &oidVal);
        JS::RootedObject oidObj(cx, oidVal.toObjectOrNull());

        // Positive control: with the matching class, the populated slot is returned. This proves
        // the reserved slot is non-null, ruling out the GetMaybePtrFromReservedSlot path below.
        ASSERT_EQ(OIDInfo::getOID(cx, oidObj, oidClass), expected);

        // Same populated object, but a mismatched checkClass. The slot is still non-null, so the
        // class check is the only thing that can produce a nil OID here.
        ASSERT_THROWS_CODE(
            OIDInfo::getOID(cx, oidObj, nonOidClass), DBException, ErrorCodes::BadValue);
    }
    setGlobalScriptEngine(nullptr);
}

// DBPointerInfo::construct defines the `ns` and `id` properties as JSPROP_READONLY, so a
// constructed DBPointer must be immutable from JavaScript. This test exercises the property
// through the public Scope interface: in sloppy mode the assignments are silently dropped and the
// original values must survive, while in strict mode the same assignments must throw a TypeError.
TEST_F(MozJSImplScopeTest, DBPointerPropertiesAreReadOnly) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    {
        std::unique_ptr<mongo::Scope> scope(
            mongo::getGlobalScriptEngine()->newScopeForCurrentThread());

        // Sloppy mode: assignments to the read-only `ns` and `id` properties are ignored, so the
        // values observed afterwards must still match the ones passed to the constructor.
        scope->exec(
            "var dbp = DBPointer('originalNs', ObjectId('507f1f77bcf86cd799439011'));"
            "var originalId = dbp.id.toString();"
            "dbp.ns = 'mutatedNs';"
            "dbp.id = ObjectId('000000000000000000000000');"
            "var nsUnchanged = (dbp.ns === 'originalNs');"
            "var idUnchanged = (dbp.id.toString() === originalId);",
            "dbPointerReadOnlySloppy",
            false /* printResult */,
            true /* reportError */,
            true /* assertOnError */);

        ASSERT_TRUE(scope->getBoolean("nsUnchanged"));
        ASSERT_TRUE(scope->getBoolean("idUnchanged"));

        // Strict mode: assigning to a read-only property must throw a TypeError for both `ns` and
        // `id`.
        scope->exec(
            "var threwForNs = false;"
            "var threwForId = false;"
            "var dbp2 = DBPointer('originalNs', ObjectId('507f1f77bcf86cd799439011'));"
            "try { (function() { 'use strict'; dbp2.ns = 'mutatedNs'; })(); }"
            "catch (e) { threwForNs = (e instanceof TypeError); }"
            "try { (function() { 'use strict'; dbp2.id = ObjectId('000000000000000000000000'); "
            "})(); }"
            "catch (e) { threwForId = (e instanceof TypeError); }",
            "dbPointerReadOnlyStrict",
            false /* printResult */,
            true /* reportError */,
            true /* assertOnError */);

        ASSERT_TRUE(scope->getBoolean("threwForNs"));
        ASSERT_TRUE(scope->getBoolean("threwForId"));
    }
    setGlobalScriptEngine(nullptr);
}

TEST_F(MozJSImplScopeTest, DeleteGlobal_RemovesInstalledGlobal) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    {
        std::unique_ptr<mongo::Scope> scope(
            mongo::getGlobalScriptEngine()->newScopeForCurrentThread());

        BSONObj doc = BSON("myFunc" << BSONCode("function() { return 1; }"));
        scope->setElement("myFunc", doc["myFunc"], doc);

        ScriptingFunction callFn = scope->createFunction("return myFunc();");
        ASSERT_EQ(0, scope->invoke(callFn, nullptr, nullptr, 0));
        ASSERT_EQ(1.0, scope->getNumber("__returnValue"));

        scope->deleteGlobal("myFunc");

        ScriptingFunction checkFn = scope->createFunction("return typeof myFunc === 'undefined';");
        ASSERT_EQ(0, scope->invoke(checkFn, nullptr, nullptr, 0));
        ASSERT_TRUE(scope->getBoolean("__returnValue"));
    }
    setGlobalScriptEngine(nullptr);
}

TEST_F(MozJSImplScopeTest, DeleteGlobal_NonExistentIsNoOp) {
    mongo::ScriptEngine::setup(ExecutionEnvironment::TestRunner);
    {
        std::unique_ptr<mongo::Scope> scope(
            mongo::getGlobalScriptEngine()->newScopeForCurrentThread());
        ASSERT_NO_THROW(scope->deleteGlobal("doesNotExist"));
    }
    setGlobalScriptEngine(nullptr);
}

}  // namespace

}  // namespace mozjs
}  // namespace mongo
