// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/shell/implscope.h"

#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/smart_ptr/shared_ptr.hpp>

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

}  // namespace

}  // namespace mozjs
}  // namespace mongo
