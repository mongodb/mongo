/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/client/remote_command_runner.h"
#include "mongo/stdx/functional.h"

namespace mongo {

    /**
     * Note: This is NOT thread-safe.
     *
     * Example usage:
     *
     * RemoteCommandRunnerMock executor;
     * executor.setNextExpectedCommand([](const RemoteCommandRequest& request) {
     *     ASSERT_EQUALS("config", request.dbname);
     * },
     * RemoteCommandResponse(BSON("ok" << 1), Milliseconds(0)));
     *
     * auto response = executor.runCommand(RemoteCommandRequest()); // Assertion error!
     */
    class RemoteCommandRunnerMock final : public RemoteCommandRunner {
    public:
        RemoteCommandRunnerMock();
        virtual ~RemoteCommandRunnerMock();

        /**
         * Shortcut for unit-tests.
         */
        static RemoteCommandRunnerMock* get(RemoteCommandRunner* runner);

        /**
         * Runs the function set by the last call to setNextExpectedCommand. Calling this more
         * than once after a single call to setNextExpectedCommand will result in an assertion
         * failure.
         *
         * Returns the value set on a previous call to setNextExpectedCommand.
         */
        StatusWith<RemoteCommandResponse> runCommand(const RemoteCommandRequest& request) override;

        /**
         * Sets the checker method to use and it's return value the next time runCommand is
         * called.
         */
        void setNextExpectedCommand(
                stdx::function<void (const RemoteCommandRequest& request)> checkerFunc,
                StatusWith<RemoteCommandResponse> returnThis);

    private:
        stdx::function<void (const RemoteCommandRequest& request)> _runCommandChecker;
        StatusWith<RemoteCommandResponse> _response;
    };
}
