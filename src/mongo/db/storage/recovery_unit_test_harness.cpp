/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/db/storage/recovery_unit_test_harness.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class RecoveryUnitTestHarness : public unittest::Test {
public:
    void setUp() override {
        harnessHelper = newRecoveryUnitHarnessHelper();
        opCtx = harnessHelper->newOperationContext();
        ru = opCtx->recoveryUnit();
    }

    std::unique_ptr<RecoveryUnitHarnessHelper> harnessHelper;
    ServiceContext::UniqueOperationContext opCtx;
    RecoveryUnit* ru;
};

TEST_F(RecoveryUnitTestHarness, CommitUnitOfWork) {
    const auto rs = harnessHelper->createRecordStore(opCtx.get(), "table1");
    ru->beginUnitOfWork(opCtx.get());
    StatusWith<RecordId> s = rs->insertRecord(opCtx.get(), "data", 4, Timestamp(), false);
    ASSERT_TRUE(s.isOK());
    ASSERT_EQUALS(1, rs->numRecords(NULL));
    ru->commitUnitOfWork();
    RecordData rd;
    ASSERT_TRUE(rs->findRecord(opCtx.get(), s.getValue(), &rd));
}

TEST_F(RecoveryUnitTestHarness, AbortUnitOfWork) {
    const auto rs = harnessHelper->createRecordStore(opCtx.get(), "table1");
    ru->beginUnitOfWork(opCtx.get());
    StatusWith<RecordId> s = rs->insertRecord(opCtx.get(), "data", 4, Timestamp(), false);
    ASSERT_TRUE(s.isOK());
    ASSERT_EQUALS(1, rs->numRecords(NULL));
    ru->abortUnitOfWork();
    ASSERT_FALSE(rs->findRecord(opCtx.get(), s.getValue(), nullptr));
}

DEATH_TEST_F(RecoveryUnitTestHarness, CommitMustBeInUnitOfWork, "invariant") {
    opCtx->recoveryUnit()->commitUnitOfWork();
}

DEATH_TEST_F(RecoveryUnitTestHarness, AbortMustBeInUnitOfWork, "invariant") {
    opCtx->recoveryUnit()->abortUnitOfWork();
}

DEATH_TEST_F(RecoveryUnitTestHarness, PrepareMustBeInUnitOfWork, "invariant") {
    opCtx->recoveryUnit()->prepareUnitOfWork();
}

DEATH_TEST_F(RecoveryUnitTestHarness, WaitUntilDurableMustBeOutOfUnitOfWork, "invariant") {
    opCtx->recoveryUnit()->beginUnitOfWork(opCtx.get());
    opCtx->recoveryUnit()->waitUntilDurable();
}

DEATH_TEST_F(RecoveryUnitTestHarness, AbandonSnapshotMustBeOutOfUnitOfWork, "invariant") {
    opCtx->recoveryUnit()->beginUnitOfWork(opCtx.get());
    opCtx->recoveryUnit()->abandonSnapshot();
}

}  // namespace
}  // namespace mongo
