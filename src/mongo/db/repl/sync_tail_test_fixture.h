/**
 *    Copyright 2017 (C) MongoDB Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/service_context_d_test_fixture.h"

namespace mongo {

class BSONObj;
class OperationContext;

namespace repl {

class ReplicationProcess;
class StorageInterfaceMock;

class SyncTailTest : public ServiceContextMongoDTest {
protected:
    void _testSyncApplyInsertDocument(ErrorCodes::Error expectedError,
                                      const BSONObj* explicitOp = nullptr);
    ServiceContext::UniqueOperationContext _opCtx;
    unsigned int _opsApplied;
    SyncTail::ApplyOperationInLockFn _applyOp;
    SyncTail::ApplyCommandInLockFn _applyCmd;
    SyncTail::IncrementOpsAppliedStatsFn _incOps;
    StorageInterfaceMock* _storageInterface = nullptr;
    ReplicationProcess* _replicationProcess = nullptr;

    // Implements the MultiApplier::ApplyOperationFn interface and does nothing.
    static Status noopApplyOperationFn(MultiApplier::OperationPtrs*) {
        return Status::OK();
    }

    OpTime nextOpTime() {
        static long long lastSecond = 1;
        return OpTime(Timestamp(Seconds(lastSecond++), 0), 1LL);
    }

    void setUp() override;
    void tearDown() override;

    Status runOpSteadyState(const OplogEntry& op);
    Status runOpInitialSync(const OplogEntry& entry);
    Status runOpsInitialSync(std::vector<OplogEntry> ops);
};

Status failedApplyCommand(OperationContext* opCtx,
                          const BSONObj& theOperation,
                          OplogApplication::Mode);

}  // namespace repl
}  // namespace mongo
