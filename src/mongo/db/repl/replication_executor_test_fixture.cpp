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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_executor_test_fixture.h"

#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/executor/network_interface_mock.h"

namespace mongo {
namespace repl {

namespace {

const int64_t prngSeed = 1;

}  // namespace

// static
Status ReplicationExecutorTest::getDetectableErrorStatus() {
    return Status(ErrorCodes::InternalError, "Not mutated");
}

void ReplicationExecutorTest::launchExecutorThread() {
    ASSERT(!_executorThread);
    _executorThread.reset(new stdx::thread(stdx::bind(&ReplicationExecutor::run, _executor.get())));
    postExecutorThreadLaunch();
}

void ReplicationExecutorTest::postExecutorThreadLaunch() {
    _net->enterNetwork();
}

void ReplicationExecutorTest::joinExecutorThread() {
    ASSERT(_executorThread);
    getNet()->exitNetwork();
    _executorThread->join();
    _executorThread.reset();
}

void ReplicationExecutorTest::setUp() {
    _net = new executor::NetworkInterfaceMock;
    _storage = new StorageInterfaceMock;
    _executor.reset(new ReplicationExecutor(_net, _storage, prngSeed));
}

void ReplicationExecutorTest::tearDown() {
    if (_executorThread) {
        _executor->shutdown();
        joinExecutorThread();
    }
    _executor.reset();
    _net = nullptr;
}

}  // namespace repl
}  // namespace mongo
