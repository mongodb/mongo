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

#include <boost/optional/optional.hpp>
#include <jsapi.h>
#include <string>

#include "mongo/scripting/deadline_monitor.h"
#include "mongo/scripting/engine.h"

namespace mongo {
namespace mozjs {

class MozJSImplScope;

/**
 * Implements the global ScriptEngine interface for MozJS.  The associated TU
 * pulls this in for the polymorphic globalScriptEngine.
 */
class MozJSScriptEngine final : public mongo::ScriptEngine {
public:
    MozJSScriptEngine(ExecutionEnvironment env);
    ~MozJSScriptEngine() override;

    void runTest() override {}

    bool utf8Ok() const override {
        return true;
    }

    void interrupt(ClientLock&, OperationContext*) override;

    void interruptAll(ServiceContextLock&) override;

    void enableJavaScriptProtection(bool value) override;
    bool isJavaScriptProtectionEnabled() const override;

    int getJSHeapLimitMB() const override;
    void setJSHeapLimitMB(int limit) override;

    std::string getLoadPath() const override;
    void setLoadPath(const std::string& loadPath) override;

    void registerOperation(OperationContext* ctx, MozJSImplScope* scope);
    void unregisterOperation(OperationContext* opCtx);

    DeadlineMonitor<MozJSImplScope>& getDeadlineMonitor() {
        return _deadlineMonitor;
    }

    ExecutionEnvironment executionEnvironment() const {
        return _executionEnvironment;
    }

protected:
    mongo::Scope* createScope() override;
    mongo::Scope* createScopeForCurrentThread(boost::optional<int> jsHeapLimitMB) override;

private:
    DeadlineMonitor<MozJSImplScope> _deadlineMonitor;
    ExecutionEnvironment _executionEnvironment;
    std::string _loadPath;
};

}  // namespace mozjs
}  // namespace mongo
