/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/concurrency/locker_noop_service_context_test_fixture.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {

class WiredTigerHarnessHelper final : public RecordStoreHarnessHelper {
public:
    WiredTigerHarnessHelper() : WiredTigerHarnessHelper(Options::ReplicationEnabled, ""_sd) {}
    WiredTigerHarnessHelper(Options options) : WiredTigerHarnessHelper(options, ""_sd) {}
    WiredTigerHarnessHelper(StringData extraStrings)
        : WiredTigerHarnessHelper(Options::ReplicationEnabled, extraStrings) {}

    WiredTigerHarnessHelper(Options options, StringData extraStrings);
    ~WiredTigerHarnessHelper() {}

    virtual std::unique_ptr<RecordStore> newRecordStore() override {
        return newRecordStore("a.b");
    }

    virtual std::unique_ptr<RecordStore> newRecordStore(const std::string& ns) {
        return newRecordStore(ns, CollectionOptions());
    }

    virtual std::unique_ptr<RecordStore> newRecordStore(
        const std::string& ns,
        const CollectionOptions& collOptions,
        KeyFormat keyFormat = KeyFormat::Long) override;

    virtual std::unique_ptr<RecordStore> newOplogRecordStore() override;

    virtual KVEngine* getEngine() override final {
        return &_engine;
    }

    std::unique_ptr<RecoveryUnit> newRecoveryUnit();

    /**
     * Create an oplog record store without calling postConstructorInit().
     */
    std::unique_ptr<RecordStore> newOplogRecordStoreNoInit();

    WT_CONNECTION* conn() {
        return _engine.getConnection();
    }

private:
    unittest::TempDir _dbpath;
    ClockSourceMock _cs;

    // Since WTKVEngine starts threads that require the global service context, we load
    // the client observer for LockerNoop before creating the storage engine to avoid a
    // potential data race (that might be reported by a tool like TSAN).
    LockerNoopClientObserverRegisterer _lockerNoopClientObserverRegisterer;
    WiredTigerKVEngine _engine;
};
}  // namespace mongo
