/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <boost/optional.hpp>

namespace mongo {

/**
 * Provides the apparatus to control the passive testing behavior and diagnostics. Testing
 * diagnostics can be controlled via the "testingDiagnosticsEnabled" server parameter, or
 * directly through calling "TestingProctor::instance().setEnabled()".
 */
class TestingProctor {
public:
    static TestingProctor& instance();

    bool isInitialized() const noexcept {
        return _diagnosticsEnabled.has_value();
    }

    /**
     * Throws "ErrorCodes::NotYetInitialized" if called before any invocation of "setEnabled()" to
     * initialize "_diagnosticsEnabled".
     */
    bool isEnabled() const;

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
