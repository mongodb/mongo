// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/deadline_monitor.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/modules.h"

#include <string>

#include <jsapi.h>

#include <boost/optional/optional.hpp>

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

    bool getJSUseLegacyMemoryTracking() const override;
    void setJSUseLegacyMemoryTracking(bool shouldUseLegacyEngine) override;

    std::string getLoadPath() const override;
    void setLoadPath(const std::string& loadPath) override;

    std::string getInterpreterVersionString() const override;

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
