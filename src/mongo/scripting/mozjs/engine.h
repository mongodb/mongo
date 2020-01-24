/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <jsapi.h>

#include "mongo/platform/mutex.h"
#include "mongo/scripting/deadline_monitor.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {
namespace mozjs {

class MozJSImplScope;

/**
 * Implements the global ScriptEngine interface for MozJS.  The associated TU
 * pulls this in for the polymorphic globalScriptEngine.
 */
class MozJSScriptEngine final : public mongo::ScriptEngine {
public:
    MozJSScriptEngine(bool disableLoadStored);
    ~MozJSScriptEngine() override;

    mongo::Scope* createScope() override;
    mongo::Scope* createScopeForCurrentThread(boost::optional<int> jsHeapLimitMB) override;

    void runTest() override {}

    bool utf8Ok() const override {
        return true;
    }

    void interrupt(unsigned opId) override;

    void interruptAll() override;

    void enableJIT(bool value) override;
    bool isJITEnabled() const override;

    void enableJavaScriptProtection(bool value) override;
    bool isJavaScriptProtectionEnabled() const override;

    int getJSHeapLimitMB() const override;
    void setJSHeapLimitMB(int limit) override;

    void registerOperation(OperationContext* ctx, MozJSImplScope* scope);
    void unregisterOperation(unsigned int opId);

    using ScopeCallback = void (*)(Scope&);
    ScopeCallback getScopeInitCallback() {
        return _scopeInitCallback;
    };

    DeadlineMonitor<MozJSImplScope>& getDeadlineMonitor() {
        return _deadlineMonitor;
    }

private:
    std::string printKnownOps_inlock();

    /**
     * This mutex protects _opToScopeMap
     */
    Mutex _globalInterruptLock = MONGO_MAKE_LATCH("MozJSScriptEngine::_globalInterruptLock");

    using OpIdToScopeMap = stdx::unordered_map<unsigned, MozJSImplScope*>;
    OpIdToScopeMap _opToScopeMap;  // map of mongo op ids to scopes (protected by
                                   // _globalInterruptLock).

    DeadlineMonitor<MozJSImplScope> _deadlineMonitor;
};

}  // namespace mozjs
}  // namespace mongo
