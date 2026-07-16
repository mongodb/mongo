// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/shell/implscope.h"

#include "mongo/bson/oid.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/types/oid.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/smart_ptr/shared_ptr.hpp>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {
namespace {

class MozJSImplScopeTest : public unittest::Test {};

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

}  // namespace

}  // namespace mozjs
}  // namespace mongo
