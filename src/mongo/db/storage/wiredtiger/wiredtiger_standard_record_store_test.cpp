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
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_oplog_stones.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using std::unique_ptr;
using std::string;
using std::stringstream;

class WiredTigerHarnessHelper final : public RecordStoreHarnessHelper {
public:
    static WT_CONNECTION* createConnection(StringData dbpath, StringData extraStrings) {
        WT_CONNECTION* conn = NULL;

        std::stringstream ss;
        ss << "create,";
        ss << "statistics=(all),";
        ss << extraStrings;
        string config = ss.str();
        int ret = wiredtiger_open(dbpath.toString().c_str(), NULL, config.c_str(), &conn);
        ASSERT_OK(wtRCToStatus(ret));
        ASSERT(conn);

        return conn;
    }

    WiredTigerHarnessHelper()
        : _dbpath("wt_test"),
          _conn(createConnection(_dbpath.path(), "")),
          _sessionCache(new WiredTigerSessionCache(_conn)) {}

    WiredTigerHarnessHelper(StringData extraStrings)
        : _dbpath("wt_test"),
          _conn(createConnection(_dbpath.path(), extraStrings)),
          _sessionCache(new WiredTigerSessionCache(_conn)) {}

    ~WiredTigerHarnessHelper() {
        delete _sessionCache;
        _conn->close(_conn, NULL);
    }

    virtual std::unique_ptr<RecordStore> newNonCappedRecordStore() {
        return newNonCappedRecordStore("a.b");
    }

    virtual std::unique_ptr<RecordStore> newNonCappedRecordStore(const std::string& ns) {
        WiredTigerRecoveryUnit* ru = new WiredTigerRecoveryUnit(_sessionCache);
        OperationContextNoop opCtx(ru);
        string uri = "table:" + ns;

        const bool prefixed = false;
        StatusWith<std::string> result = WiredTigerRecordStore::generateCreateString(
            kWiredTigerEngineName, ns, CollectionOptions(), "", prefixed);
        ASSERT_TRUE(result.isOK());
        std::string config = result.getValue();

        {
            WriteUnitOfWork uow(&opCtx);
            WT_SESSION* s = ru->getSession(&opCtx)->getSession();
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

        auto ret = stdx::make_unique<StandardWiredTigerRecordStore>(&opCtx, params);
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
        WiredTigerRecoveryUnit* ru = new WiredTigerRecoveryUnit(_sessionCache);
        OperationContextNoop opCtx(ru);
        string uri = "table:a.b";

        CollectionOptions options;
        options.capped = true;

        const bool prefixed = false;
        StatusWith<std::string> result = WiredTigerRecordStore::generateCreateString(
            kWiredTigerEngineName, ns, options, "", prefixed);
        ASSERT_TRUE(result.isOK());
        std::string config = result.getValue();

        {
            WriteUnitOfWork uow(&opCtx);
            WT_SESSION* s = ru->getSession(&opCtx)->getSession();
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

        auto ret = stdx::make_unique<StandardWiredTigerRecordStore>(&opCtx, params);
        ret->postConstructorInit(&opCtx);
        return std::move(ret);
    }

    virtual std::unique_ptr<RecoveryUnit> newRecoveryUnit() final {
        return stdx::make_unique<WiredTigerRecoveryUnit>(_sessionCache);
    }

    virtual bool supportsDocLocking() final {
        return true;
    }

    virtual WT_CONNECTION* conn() const {
        return _conn;
    }

private:
    unittest::TempDir _dbpath;
    WT_CONNECTION* _conn;
    WiredTigerSessionCache* _sessionCache;
};

std::unique_ptr<HarnessHelper> makeHarnessHelper() {
    return stdx::make_unique<WiredTigerHarnessHelper>();
}

MONGO_INITIALIZER(RegisterHarnessFactory)(InitializerContext* const) {
    mongo::registerHarnessHelperFactory(makeHarnessHelper);
    return Status::OK();
}

TEST(WiredTigerRecordStoreTest, StorageSizeStatisticsDisabled) {
    WiredTigerHarnessHelper harnessHelper("statistics=(none)");
    unique_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("a.b"));

    ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
    ASSERT_THROWS(rs->storageSize(opCtx.get()), UserException);
}

TEST(WiredTigerRecordStoreTest, SizeStorer1) {
    unique_ptr<WiredTigerHarnessHelper> harnessHelper(new WiredTigerHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    string uri = checked_cast<WiredTigerRecordStore*>(rs.get())->getURI();

    string indexUri = "table:myindex";
    const bool enableWtLogging = false;
    WiredTigerSizeStorer ss(harnessHelper->conn(), indexUri, enableWtLogging);
    checked_cast<WiredTigerRecordStore*>(rs.get())->setSizeStorer(&ss);

    int N = 12;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            for (int i = 0; i < N; i++) {
                StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, false);
                ASSERT_OK(res.getStatus());
            }
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(N, rs->numRecords(opCtx.get()));
    }

    rs.reset(NULL);

    {
        long long numRecords;
        long long dataSize;
        ss.loadFromCache(uri, &numRecords, &dataSize);
        ASSERT_EQUALS(N, numRecords);
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WiredTigerRecordStore::Params params;
        params.ns = "a.b"_sd;
        params.uri = uri;
        params.engineName = kWiredTigerEngineName;
        params.isCapped = false;
        params.isEphemeral = false;
        params.cappedMaxSize = -1;
        params.cappedMaxDocs = -1;
        params.cappedCallback = nullptr;
        params.sizeStorer = &ss;

        auto ret = new StandardWiredTigerRecordStore(opCtx.get(), params);
        ret->postConstructorInit(opCtx.get());
        rs.reset(ret);
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(N, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WiredTigerRecoveryUnit* ru = checked_cast<WiredTigerRecoveryUnit*>(opCtx->recoveryUnit());

        {
            WriteUnitOfWork uow(opCtx.get());
            WT_SESSION* s = ru->getSession(opCtx.get())->getSession();
            invariantWTOK(s->create(s, indexUri.c_str(), ""));
            uow.commit();
        }

        ss.syncCache(true);
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const bool enableWtLogging = false;
        WiredTigerSizeStorer ss2(harnessHelper->conn(), indexUri, enableWtLogging);
        ss2.fillCache();
        long long numRecords;
        long long dataSize;
        ss2.loadFromCache(uri, &numRecords, &dataSize);
        ASSERT_EQUALS(N, numRecords);
    }

    rs.reset(NULL);  // this has to be deleted before ss
}

class GoodValidateAdaptor : public ValidateAdaptor {
public:
    virtual Status validate(const RecordId& recordId, const RecordData& record, size_t* dataSize) {
        *dataSize = static_cast<size_t>(record.size());
        return Status::OK();
    }
};

class BadValidateAdaptor : public ValidateAdaptor {
public:
    virtual Status validate(const RecordId& recordId, const RecordData& record, size_t* dataSize) {
        *dataSize = static_cast<size_t>(record.size());
        return Status(ErrorCodes::UnknownError, "");
    }
};

class SizeStorerValidateTest : public mongo::unittest::Test {
private:
    virtual void setUp() {
        harnessHelper.reset(new WiredTigerHarnessHelper());
        const bool enableWtLogging = false;
        sizeStorer.reset(
            new WiredTigerSizeStorer(harnessHelper->conn(), "table:sizeStorer", enableWtLogging));
        rs = harnessHelper->newNonCappedRecordStore();
        WiredTigerRecordStore* wtrs = checked_cast<WiredTigerRecordStore*>(rs.get());
        wtrs->setSizeStorer(sizeStorer.get());
        uri = wtrs->getURI();

        expectedNumRecords = 10000;
        expectedDataSize = expectedNumRecords * 2;
        {
            ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
            WriteUnitOfWork uow(opCtx.get());
            for (int i = 0; i < expectedNumRecords; i++) {
                ASSERT_OK(rs->insertRecord(opCtx.get(), "a", 2, false).getStatus());
            }
            uow.commit();
        }
        ASSERT_EQUALS(expectedNumRecords, rs->numRecords(NULL));
        ASSERT_EQUALS(expectedDataSize, rs->dataSize(NULL));
        sizeStorer->storeToCache(uri, 0, 0);
    }
    virtual void tearDown() {
        expectedNumRecords = 0;
        expectedDataSize = 0;

        rs.reset(NULL);
        sizeStorer.reset(NULL);
        harnessHelper.reset(NULL);
        rs.reset(NULL);
    }

protected:
    long long getNumRecords() const {
        long long numRecords;
        long long unused;
        sizeStorer->loadFromCache(uri, &numRecords, &unused);
        return numRecords;
    }

    long long getDataSize() const {
        long long unused;
        long long dataSize;
        sizeStorer->loadFromCache(uri, &unused, &dataSize);
        return dataSize;
    }

    std::unique_ptr<WiredTigerHarnessHelper> harnessHelper;
    std::unique_ptr<WiredTigerSizeStorer> sizeStorer;
    std::unique_ptr<RecordStore> rs;
    std::string uri;

    long long expectedNumRecords;
    long long expectedDataSize;
};

// Basic validation - size storer data is updated.
TEST_F(SizeStorerValidateTest, Basic) {
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    GoodValidateAdaptor adaptor;
    ValidateResults results;
    BSONObjBuilder output;
    ASSERT_OK(rs->validate(opCtx.get(), kValidateIndex, &adaptor, &results, &output));
    BSONObj obj = output.obj();
    ASSERT_EQUALS(expectedNumRecords, obj.getIntField("nrecords"));
    ASSERT_EQUALS(expectedNumRecords, getNumRecords());
    ASSERT_EQUALS(expectedDataSize, getDataSize());
}

// Full validation - size storer data is updated.
TEST_F(SizeStorerValidateTest, FullWithGoodAdaptor) {
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    GoodValidateAdaptor adaptor;
    ValidateResults results;
    BSONObjBuilder output;
    ASSERT_OK(rs->validate(opCtx.get(), kValidateFull, &adaptor, &results, &output));
    BSONObj obj = output.obj();
    ASSERT_EQUALS(expectedNumRecords, obj.getIntField("nrecords"));
    ASSERT_EQUALS(expectedNumRecords, getNumRecords());
    ASSERT_EQUALS(expectedDataSize, getDataSize());
}

// Full validation with a validation adaptor that fails - size storer data is not updated.
TEST_F(SizeStorerValidateTest, FullWithBadAdapter) {
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    BadValidateAdaptor adaptor;
    ValidateResults results;
    BSONObjBuilder output;
    ASSERT_OK(rs->validate(opCtx.get(), kValidateFull, &adaptor, &results, &output));
    BSONObj obj = output.obj();
    ASSERT_EQUALS(expectedNumRecords, obj.getIntField("nrecords"));
    ASSERT_EQUALS(0, getNumRecords());
    ASSERT_EQUALS(0, getDataSize());
}

// Load bad _numRecords and _dataSize values at record store creation.
TEST_F(SizeStorerValidateTest, InvalidSizeStorerAtCreation) {
    rs.reset(NULL);

    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    sizeStorer->storeToCache(uri, expectedNumRecords * 2, expectedDataSize * 2);

    WiredTigerRecordStore::Params params;
    params.ns = "a.b"_sd;
    params.uri = uri;
    params.engineName = kWiredTigerEngineName;
    params.isCapped = false;
    params.isEphemeral = false;
    params.cappedMaxSize = -1;
    params.cappedMaxDocs = -1;
    params.cappedCallback = nullptr;
    params.sizeStorer = sizeStorer.get();

    auto ret = new StandardWiredTigerRecordStore(opCtx.get(), params);
    ret->postConstructorInit(opCtx.get());
    rs.reset(ret);

    ASSERT_EQUALS(expectedNumRecords * 2, rs->numRecords(NULL));
    ASSERT_EQUALS(expectedDataSize * 2, rs->dataSize(NULL));

    // Full validation should fix record and size counters.
    GoodValidateAdaptor adaptor;
    ValidateResults results;
    BSONObjBuilder output;
    ASSERT_OK(rs->validate(opCtx.get(), kValidateFull, &adaptor, &results, &output));
    BSONObj obj = output.obj();
    ASSERT_EQUALS(expectedNumRecords, obj.getIntField("nrecords"));
    ASSERT_EQUALS(expectedNumRecords, getNumRecords());
    ASSERT_EQUALS(expectedDataSize, getDataSize());

    ASSERT_EQUALS(expectedNumRecords, rs->numRecords(NULL));
    ASSERT_EQUALS(expectedDataSize, rs->dataSize(NULL));
}

}  // namespace
}  // mongo
