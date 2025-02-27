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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_kv_engine.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"
#include "mongo/db/storage/recovery_unit_test_harness.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace ephemeral_for_test {
namespace {

class RecoveryUnitHarnessHelper final : public mongo::RecoveryUnitHarnessHelper {
public:
    RecoveryUnitHarnessHelper() = default;

    virtual std::unique_ptr<mongo::RecoveryUnit> newRecoveryUnit() final {
        return std::make_unique<RecoveryUnit>(&_kvEngine);
    }

    virtual std::unique_ptr<mongo::RecordStore> createRecordStore(OperationContext* opCtx,
                                                                  const std::string& ns) {
        return std::make_unique<RecordStore>(ns,
                                             "ident"_sd /* ident */,
                                             KeyFormat::Long,
                                             false /* isCapped */,
                                             nullptr /* cappedCallback */);
    }

private:
    KVEngine _kvEngine{};
};

std::unique_ptr<mongo::RecoveryUnitHarnessHelper> makeRecoveryUnitHarnessHelper() {
    return std::make_unique<RecoveryUnitHarnessHelper>();
}

MONGO_INITIALIZER(RegisterRecoveryUnitHarnessFactory)(InitializerContext* const) {
    mongo::registerRecoveryUnitHarnessHelperFactory(makeRecoveryUnitHarnessHelper);
}

class EphemeralForTestRecoveryUnitTestHarness : public unittest::Test {
public:
    void setUp() override {
        harnessHelper = makeRecoveryUnitHarnessHelper();
        opCtx = harnessHelper->newOperationContext();
        ru = opCtx->recoveryUnit();
    }

    std::unique_ptr<mongo::RecoveryUnitHarnessHelper> harnessHelper;
    ServiceContext::UniqueOperationContext opCtx;
    mongo::RecoveryUnit* ru;
};

TEST_F(EphemeralForTestRecoveryUnitTestHarness, AbandonSnapshotAbortMode) {
    Lock::GlobalLock globalLk(opCtx.get(), MODE_IX);

    ru->setAbandonSnapshotMode(RecoveryUnit::AbandonSnapshotMode::kAbort);

    const auto rs = harnessHelper->createRecordStore(opCtx.get(), "table1");
    opCtx->lockState()->beginWriteUnitOfWork();
    ru->beginUnitOfWork(opCtx->readOnly());
    StatusWith<RecordId> rid1 = rs->insertRecord(opCtx.get(), "ABC", 3, Timestamp());
    StatusWith<RecordId> rid2 = rs->insertRecord(opCtx.get(), "123", 3, Timestamp());
    ASSERT_TRUE(rid1.isOK());
    ASSERT_TRUE(rid2.isOK());
    ASSERT_EQUALS(2, rs->numRecords(opCtx.get()));
    ru->commitUnitOfWork();
    opCtx->lockState()->endWriteUnitOfWork();

    auto snapshotIdBefore = ru->getSnapshotId();

    // Now create a cursor.
    auto cursor = rs->getCursor(opCtx.get());

    auto record = cursor->next();
    ASSERT(record);
    ASSERT_EQ(record->id, rid1.getValue());

    // Abandon the snapshot.
    ru->abandonSnapshot();
    ASSERT_NE(snapshotIdBefore, ru->getSnapshotId());

    // After the snapshot is abandoned, calls to next() will simply use a newer snapshot.
    // This behavior is specific to EFT, and other engines may behave differently.
    auto record2 = cursor->next();
    ASSERT(record2);
    ASSERT_EQ(record2->id, rid2.getValue());
}
}  // namespace
}  // namespace ephemeral_for_test
}  // namespace mongo
