// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>

#include <wiredtiger.h>

namespace mongo {
using namespace std::literals::string_view_literals;

class WiredTigerHarnessHelper final : public RecordStoreHarnessHelper {
public:
    WiredTigerHarnessHelper() : WiredTigerHarnessHelper(Options::ReplicationEnabled, ""sv) {}
    WiredTigerHarnessHelper(Options options) : WiredTigerHarnessHelper(options, ""sv) {}
    WiredTigerHarnessHelper(std::string_view extraStrings)
        : WiredTigerHarnessHelper(Options::ReplicationEnabled, extraStrings) {}

    WiredTigerHarnessHelper(Options options, std::string_view extraStrings);
    ~WiredTigerHarnessHelper() override {
#if __has_feature(address_sanitizer)
        constexpr bool memLeakAllowed = false;
#else
        constexpr bool memLeakAllowed = true;
#endif
        _engine->getOplogManager()->stop(_oplog.get());
        _engine->cleanShutdown(memLeakAllowed);
    }

    std::unique_ptr<RecordStore> newRecordStore(
        const RecordStore::Options& rsOptions = RecordStore::Options{}) override {
        return newRecordStore("a.b", rsOptions);
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
                                                std::string_view ident,
                                                const RecordStore::Options& recordStoreOptions,
                                                boost::optional<UUID> uuid);

    RecordStore& oplogRecordStore() override;

    KVEngine* getEngine() final {
        return _engine.get();
    }

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() override;

    WiredTigerConnection& connection() {
        return _engine->getConnection();
    }

    WT_CONNECTION* conn() {
        return _engine->getConn();
    }

private:
    std::string _identForNs(std::string_view ns) {
        auto ident = fmt::format("collection-{}", ns);
        std::replace(ident.begin(), ident.end(), '.', '_');
        return ident;
    }

    unittest::TempDir _dbpath;
    ClockSourceMock _cs;
    std::unique_ptr<WiredTigerKVEngine> _engine;
    std::unique_ptr<RecordStore> _oplog;
    bool _isReplSet;
};
}  // namespace mongo
