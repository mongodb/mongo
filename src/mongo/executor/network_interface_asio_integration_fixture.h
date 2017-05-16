/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/unittest/unittest.h"

#include "mongo/executor/network_interface_asio.h"
#include "mongo/executor/network_interface_asio_test_utils.h"
#include "mongo/executor/task_executor.h"

namespace mongo {

class PseudoRandom;

namespace executor {

using StartCommandCB = stdx::function<void(const RemoteCommandResponse&)>;

class NetworkInterfaceASIOIntegrationFixture : public mongo::unittest::Test {
public:
    void startNet(NetworkInterfaceASIO::Options options = NetworkInterfaceASIO::Options());
    void tearDown() override;

    NetworkInterfaceASIO& net();

    ConnectionString fixture();

    void setRandomNumberGenerator(PseudoRandom* generator);

    PseudoRandom* getRandomNumberGenerator();

    void startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                      RemoteCommandRequest& request,
                      StartCommandCB onFinish);

    Deferred<RemoteCommandResponse> runCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                               RemoteCommandRequest& request);

    RemoteCommandResponse runCommandSync(RemoteCommandRequest& request);

    void assertCommandOK(StringData db,
                         const BSONObj& cmd,
                         Milliseconds timeoutMillis = Minutes(5));
    void assertCommandFailsOnClient(StringData db,
                                    const BSONObj& cmd,
                                    ErrorCodes::Error reason,
                                    Milliseconds timeoutMillis = Minutes(5));

    void assertCommandFailsOnServer(StringData db,
                                    const BSONObj& cmd,
                                    ErrorCodes::Error reason,
                                    Milliseconds timeoutMillis = Minutes(5));

    void assertWriteError(StringData db,
                          const BSONObj& cmd,
                          ErrorCodes::Error reason,
                          Milliseconds timeoutMillis = Minutes(5));

private:
    std::unique_ptr<NetworkInterfaceASIO> _net;
    PseudoRandom* _rng = nullptr;
};
}  // namespace executor
}  // namespace mongo
