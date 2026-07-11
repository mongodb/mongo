// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/static_immortal.h"

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
/**
 * Provides the apparatus to control the passive testing behavior and diagnostics. Testing
 * diagnostics can be controlled via the "testingDiagnosticsEnabled" server parameter, or
 * directly through calling "TestingProctor::instance().setEnabled()".
 */
class TestingProctor {
public:
    static TestingProctor& instance() {
        static StaticImmortal<TestingProctor> proctor{};
        return proctor.value();
    }

    bool isInitialized() const noexcept {
        return MONGO_likely(_diagnosticsEnabled.has_value());
    }

    /**
     * Throws "ErrorCodes::NotYetInitialized" if called before any invocation of "setEnabled()" to
     * initialize "_diagnosticsEnabled".
     */
    bool isEnabled() const {
        uassert(ErrorCodes::NotYetInitialized,
                "Cannot check whether testing diagnostics is enabled before it is initialized",
                isInitialized());
        return MONGO_unlikely(_diagnosticsEnabled.value());
    }

    /**
     * Enables/disables testing diagnostics. Once invoked for the first time during the lifetime of
     * a process, its impact (i.e., enabled or disabled diagnostics) cannot be altered. Throws
     * "ErrorCodes::AlreadyInitialized" if the caller provides a value for "enable" that does not
     * match what is stored in "_diagnosticsEnabled".
     */
    void setEnabled(bool enable);

    /**
     * Quick exits with ExitCode::abrupt if any deferred errors have occurred.
     */
    void exitAbruptlyIfDeferredErrors(bool verbose = true) const;

private:
    boost::optional<bool> _diagnosticsEnabled;
};

}  // namespace mongo
