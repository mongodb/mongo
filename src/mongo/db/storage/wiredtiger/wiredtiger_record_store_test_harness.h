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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/clock_source_mock.h"

#include <memory>
#include <string>

#include <wiredtiger.h>

namespace mongo {

class WiredTigerHarnessHelper final : public RecordStoreHarnessHelper {
public:
    WiredTigerHarnessHelper() : WiredTigerHarnessHelper(Options::ReplicationEnabled, ""_sd) {}
    WiredTigerHarnessHelper(Options options) : WiredTigerHarnessHelper(options, ""_sd) {}
    WiredTigerHarnessHelper(StringData extraStrings)
        : WiredTigerHarnessHelper(Options::ReplicationEnabled, extraStrings) {}

    WiredTigerHarnessHelper(Options options, StringData extraStrings);
    ~WiredTigerHarnessHelper() override {
#if __has_feature(address_sanitizer)
        constexpr bool memLeakAllowed = false;
#else
        constexpr bool memLeakAllowed = true;
#endif
        _engine->cleanShutdown(memLeakAllowed);
    }

    std::unique_ptr<RecordStore> newRecordStore() override {
        return newRecordStore("a.b");
    }

    virtual std::unique_ptr<RecordStore> newRecordStore(const std::string& ns) {
        return newRecordStore(ns, RecordStore::Options{});
    }

    std::unique_ptr<RecordStore> newRecordStore(
        const std::string& ns, const RecordStore::Options& recordStoreOptions) override {
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
        return newRecordStore(nss, _identForNs(ns), recordStoreOptions, UUID::gen());
    }

    std::unique_ptr<RecordStore> newRecordStore(const NamespaceString& nss,
                                                StringData ident,
                                                const RecordStore::Options& recordStoreOptions,
                                                boost::optional<UUID> uuid);

    std::unique_ptr<RecordStore> newOplogRecordStore() override;

    KVEngine* getEngine() final {
        return _engine.get();
    }

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() override;

    /**
     * Create an oplog record store without starting the oplog manager.
     */
    std::unique_ptr<RecordStore> newOplogRecordStoreNoInit();

    WiredTigerConnection& connection() {
        return _engine->getConnection();
    }

    WT_CONNECTION* conn() {
        return _engine->getConn();
    }

private:
    std::string _identForNs(StringData ns) {
        auto ident = fmt::format("collection-{}", ns);
        std::replace(ident.begin(), ident.end(), '.', '_');
        return ident;
    }

    unittest::TempDir _dbpath;
    ClockSourceMock _cs;
    std::unique_ptr<WiredTigerKVEngine> _engine;
    bool _isReplSet;
};
}  // namespace mongo
