/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <memory>
#include <sstream>
#include <string>
#include <time.h>

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/kv/kv_engine_test_harness.h"
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_oplog_stones.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using std::unique_ptr;
using std::string;
using std::stringstream;

class PrefixedWiredTigerHarnessHelper final : public RecordStoreHarnessHelper {
public:
    PrefixedWiredTigerHarnessHelper()
        : _dbpath("wt_test"),
          _engine(new WiredTigerKVEngine(kWiredTigerEngineName,
                                         _dbpath.path(),
                                         _cs.get(),
                                         "",
                                         1,
                                         false,
                                         false,
                                         false,
                                         false)) {}

    PrefixedWiredTigerHarnessHelper(StringData extraStrings) : _dbpath("wt_test") {}

    virtual std::unique_ptr<RecordStore> newNonCappedRecordStore() {
        return newNonCappedRecordStore("a.b");
    }

    virtual std::unique_ptr<RecordStore> newNonCappedRecordStore(const std::string& ns) {
        WiredTigerRecoveryUnit* ru =
            checked_cast<WiredTigerRecoveryUnit*>(_engine->newRecoveryUnit());
        OperationContextNoop opCtx(ru);
        string uri = "table:" + ns;

        const bool prefixed = true;
        StatusWith<std::string> result = WiredTigerRecordStore::generateCreateString(
            kWiredTigerEngineName, ns, CollectionOptions(), "", prefixed);
        ASSERT_TRUE(result.isOK());
        std::string config = result.getValue();

        {
            WriteUnitOfWork uow(&opCtx);
            WT_SESSION* s = ru->getSession()->getSession();
            invariantWTOK(s->create(s, uri.c_str(), config.c_str()));
            uow.commit();
        }

        WiredTigerRecordStore::Params params;
        params.ns = ns;
        params.uri = uri;
        params.engineName = kWiredTigerEngineName;
        params.isCapped = false;
        params.isEphemeral = false;
        params.cappedMaxSize = -1;
        params.cappedMaxDocs = -1;
        params.cappedCallback = nullptr;
        params.sizeStorer = nullptr;

        auto ret = stdx::make_unique<PrefixedWiredTigerRecordStore>(
            _engine.get(), &opCtx, params, KVPrefix::generateNextPrefix());
        ret->postConstructorInit(&opCtx);
        return std::move(ret);
    }

    virtual std::unique_ptr<RecordStore> newCappedRecordStore(int64_t cappedSizeBytes,
                                                              int64_t cappedMaxDocs) final {
        return newCappedRecordStore("a.b", cappedSizeBytes, cappedMaxDocs);
    }

    virtual std::unique_ptr<RecordStore> newCappedRecordStore(const std::string& ns,
                                                              int64_t cappedMaxSize,
                                                              int64_t cappedMaxDocs) {
        WiredTigerRecoveryUnit* ru =
            checked_cast<WiredTigerRecoveryUnit*>(_engine->newRecoveryUnit());
        OperationContextNoop opCtx(ru);
        string uri = "table:a.b";

        CollectionOptions options;
        options.capped = true;

        KVPrefix prefix = KVPrefix::generateNextPrefix();
        StatusWith<std::string> result = WiredTigerRecordStore::generateCreateString(
            kWiredTigerEngineName, ns, options, "", prefix.isPrefixed());
        ASSERT_TRUE(result.isOK());
        std::string config = result.getValue();

        {
            WriteUnitOfWork uow(&opCtx);
            WT_SESSION* s = ru->getSession()->getSession();
            invariantWTOK(s->create(s, uri.c_str(), config.c_str()));
            uow.commit();
        }

        WiredTigerRecordStore::Params params;
        params.ns = ns;
        params.uri = uri;
        params.engineName = kWiredTigerEngineName;
        params.isCapped = true;
        params.isEphemeral = false;
        params.cappedMaxSize = cappedMaxSize;
        params.cappedMaxDocs = cappedMaxDocs;
        params.cappedCallback = nullptr;
        params.sizeStorer = nullptr;

        auto ret =
            stdx::make_unique<PrefixedWiredTigerRecordStore>(_engine.get(), &opCtx, params, prefix);
        ret->postConstructorInit(&opCtx);
        return std::move(ret);
    }

    virtual std::unique_ptr<RecoveryUnit> newRecoveryUnit() final {
        return std::unique_ptr<WiredTigerRecoveryUnit>(
            checked_cast<WiredTigerRecoveryUnit*>(_engine->newRecoveryUnit()));
    }

    virtual bool supportsDocLocking() final {
        return true;
    }

    virtual WT_CONNECTION* conn() const {
        return _engine->getConnection();
    }

private:
    unittest::TempDir _dbpath;
    const std::unique_ptr<ClockSource> _cs = stdx::make_unique<ClockSourceMock>();

    std::unique_ptr<WiredTigerKVEngine> _engine;
};

std::unique_ptr<HarnessHelper> makeHarnessHelper() {
    return stdx::make_unique<PrefixedWiredTigerHarnessHelper>();
}

MONGO_INITIALIZER(RegisterHarnessFactory)(InitializerContext* const) {
    mongo::registerHarnessHelperFactory(makeHarnessHelper);
    return Status::OK();
}

TEST(WiredTigerRecordStoreTest, PrefixedTableScan) {
    unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    unique_ptr<RecordStore> rs = harnessHelper->newNonCappedRecordStore("a.b");

    const int numDocs = 1000;
    {  // Insert documents.
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        for (int num = 0; num < numDocs; ++num) {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp(), false);
            ASSERT_OK(res.getStatus());
            uow.commit();
        }
    }

    auto client = harnessHelper->serviceContext()->makeClient("client");
    auto cursorCtx = harnessHelper->newOperationContext(client.get());

    auto cursor = rs->getCursor(cursorCtx.get());
    for (int num = 0; num < numDocs; ++num) {
        ASSERT(cursor->next());
    }
    ASSERT(!cursor->next());
}

TEST(WiredTigerRecordStoreTest, PrefixedSeekingCursor) {
    unique_ptr<RecordStoreHarnessHelper> harnessHelper = newRecordStoreHarnessHelper();
    unique_ptr<RecordStore> rs = harnessHelper->newNonCappedRecordStore("a.b");

    RecordId startRecordId;
    const int numDocs = 1000;
    {  // Insert documents.
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        for (int num = 0; num < numDocs; ++num) {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, Timestamp(), false);
            if (startRecordId.isNull()) {
                startRecordId = res.getValue();
            }
            ASSERT_OK(res.getStatus());
            uow.commit();
        }
    }

    auto client = harnessHelper->serviceContext()->makeClient("client");
    auto cursorCtx = harnessHelper->newOperationContext(client.get());

    auto cursor = rs->getCursor(cursorCtx.get());
    for (int num = 0; num < numDocs; ++num) {
        ASSERT(cursor->seekExact(RecordId(startRecordId.repr() + num)));
    }
    ASSERT(!cursor->seekExact(RecordId(startRecordId.repr() + numDocs)));
}

}  // namespace
}  // mongo
