// wiredtiger_record_store_test.cpp

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

#include <sstream>
#include <string>

#include "mongo/base/checked_cast.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_oplog_stones.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::unique_ptr;
using std::string;
using std::stringstream;

class WiredTigerHarnessHelper final : public HarnessHelper {
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
    std::unique_ptr<RecordStore> newNonCappedRecordStore(const std::string& ns) {
        WiredTigerRecoveryUnit* ru = new WiredTigerRecoveryUnit(_sessionCache);
        OperationContextNoop txn(ru);
        string uri = "table:" + ns;

        StatusWith<std::string> result = WiredTigerRecordStore::generateCreateString(
            kWiredTigerEngineName, ns, CollectionOptions(), "");
        ASSERT_TRUE(result.isOK());
        std::string config = result.getValue();

        {
            WriteUnitOfWork uow(&txn);
            WT_SESSION* s = ru->getSession(&txn)->getSession();
            invariantWTOK(s->create(s, uri.c_str(), config.c_str()));
            uow.commit();
        }

        return stdx::make_unique<WiredTigerRecordStore>(
            &txn, ns, uri, kWiredTigerEngineName, false, false);
    }

    std::unique_ptr<RecordStore> newCappedRecordStore(int64_t cappedSizeBytes,
                                                      int64_t cappedMaxDocs) final {
        return newCappedRecordStore("a.b", cappedSizeBytes, cappedMaxDocs);
    }

    std::unique_ptr<RecordStore> newCappedRecordStore(const std::string& ns,
                                                      int64_t cappedMaxSize,
                                                      int64_t cappedMaxDocs) {
        WiredTigerRecoveryUnit* ru = new WiredTigerRecoveryUnit(_sessionCache);
        OperationContextNoop txn(ru);
        string uri = "table:a.b";

        CollectionOptions options;
        options.capped = true;

        StatusWith<std::string> result =
            WiredTigerRecordStore::generateCreateString(kWiredTigerEngineName, ns, options, "");
        ASSERT_TRUE(result.isOK());
        std::string config = result.getValue();

        {
            WriteUnitOfWork uow(&txn);
            WT_SESSION* s = ru->getSession(&txn)->getSession();
            invariantWTOK(s->create(s, uri.c_str(), config.c_str()));
            uow.commit();
        }

        return stdx::make_unique<WiredTigerRecordStore>(
            &txn, ns, uri, kWiredTigerEngineName, true, false, cappedMaxSize, cappedMaxDocs);
    }

    RecoveryUnit* newRecoveryUnit() final {
        return new WiredTigerRecoveryUnit(_sessionCache);
    }

    bool supportsDocLocking() final {
        return true;
    }

    WT_CONNECTION* conn() const {
        return _conn;
    }

private:
    unittest::TempDir _dbpath;
    WT_CONNECTION* _conn;
    WiredTigerSessionCache* _sessionCache;
};

std::unique_ptr<HarnessHelper> newHarnessHelper() {
    return stdx::make_unique<WiredTigerHarnessHelper>();
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringEmptyDocument) {
    BSONObj spec = fromjson("{}");
    StatusWith<std::string> result = WiredTigerRecordStore::parseOptionsField(spec);
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), "");  // "," would also be valid.
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringUnknownField) {
    BSONObj spec = fromjson("{unknownField: 1}");
    StatusWith<std::string> result = WiredTigerRecordStore::parseOptionsField(spec);
    const Status& status = result.getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, status);
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringNonStringConfig) {
    BSONObj spec = fromjson("{configString: 12345}");
    StatusWith<std::string> result = WiredTigerRecordStore::parseOptionsField(spec);
    const Status& status = result.getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringEmptyConfigString) {
    BSONObj spec = fromjson("{configString: ''}");
    StatusWith<std::string> result = WiredTigerRecordStore::parseOptionsField(spec);
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(result.getValue(), ",");  // "" would also be valid.
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringInvalidConfigStringOption) {
    BSONObj spec = fromjson("{configString: 'abc=def'}");
    ASSERT_EQ(WiredTigerRecordStore::parseOptionsField(spec), ErrorCodes::BadValue);
}

TEST(WiredTigerRecordStoreTest, GenerateCreateStringValidConfigStringOption) {
    BSONObj spec = fromjson("{configString: 'prefix_compression=true'}");
    ASSERT_EQ(WiredTigerRecordStore::parseOptionsField(spec),
              std::string("prefix_compression=true,"));
}

TEST(WiredTigerRecordStoreTest, Isolation1) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    RecordId id1;
    RecordId id2;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());

            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            id1 = res.getValue();

            res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            id2 = res.getValue();

            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext t1(harnessHelper->newOperationContext());
        auto client2 = harnessHelper->serviceContext()->makeClient("c2");
        auto t2 = harnessHelper->newOperationContext(client2.get());

        unique_ptr<WriteUnitOfWork> w1(new WriteUnitOfWork(t1.get()));
        unique_ptr<WriteUnitOfWork> w2(new WriteUnitOfWork(t2.get()));

        rs->dataFor(t1.get(), id1);
        rs->dataFor(t2.get(), id1);

        ASSERT_OK(rs->updateRecord(t1.get(), id1, "b", 2, false, NULL));
        ASSERT_OK(rs->updateRecord(t1.get(), id2, "B", 2, false, NULL));

        try {
            // this should fail
            rs->updateRecord(t2.get(), id1, "c", 2, false, NULL);
            ASSERT(0);
        } catch (WriteConflictException& dle) {
            w2.reset(NULL);
            t2.reset(NULL);
        }

        w1->commit();  // this should succeed
    }
}

TEST(WiredTigerRecordStoreTest, Isolation2) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    RecordId id1;
    RecordId id2;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());

            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            id1 = res.getValue();

            res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            id2 = res.getValue();

            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext t1(harnessHelper->newOperationContext());
        auto client2 = harnessHelper->serviceContext()->makeClient("c2");
        auto t2 = harnessHelper->newOperationContext(client2.get());

        // ensure we start transactions
        rs->dataFor(t1.get(), id2);
        rs->dataFor(t2.get(), id2);

        {
            WriteUnitOfWork w(t1.get());
            ASSERT_OK(rs->updateRecord(t1.get(), id1, "b", 2, false, NULL));
            w.commit();
        }

        {
            WriteUnitOfWork w(t2.get());
            ASSERT_EQUALS(string("a"), rs->dataFor(t2.get(), id1).data());
            try {
                // this should fail as our version of id1 is too old
                rs->updateRecord(t2.get(), id1, "c", 2, false, NULL);
                ASSERT(0);
            } catch (WriteConflictException& dle) {
            }
        }
    }
}

TEST(WiredTigerRecordStoreTest, SizeStorer1) {
    unique_ptr<WiredTigerHarnessHelper> harnessHelper(new WiredTigerHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    string uri = checked_cast<WiredTigerRecordStore*>(rs.get())->getURI();

    string indexUri = "table:myindex";
    WiredTigerSizeStorer ss(harnessHelper->conn(), indexUri);
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
        rs.reset(new WiredTigerRecordStore(
            opCtx.get(), "a.b", uri, kWiredTigerEngineName, false, false, -1, -1, NULL, &ss));
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
        WiredTigerSizeStorer ss2(harnessHelper->conn(), indexUri);
        ss2.fillCache();
        long long numRecords;
        long long dataSize;
        ss2.loadFromCache(uri, &numRecords, &dataSize);
        ASSERT_EQUALS(N, numRecords);
    }

    rs.reset(NULL);  // this has to be deleted before ss
}

namespace {

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
        sizeStorer.reset(new WiredTigerSizeStorer(harnessHelper->conn(), "table:sizeStorer"));
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
    ValidateResults results;
    BSONObjBuilder output;
    ASSERT_OK(rs->validate(opCtx.get(), kValidateIndex, NULL, &results, &output));
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

// Basic validation does not use the validation adaptor. So passing a bad adaptor
// should not cause validate to fail.
TEST_F(SizeStorerValidateTest, BasicWithBadAdapter) {
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    BadValidateAdaptor adaptor;
    ValidateResults results;
    BSONObjBuilder output;
    ASSERT_OK(rs->validate(opCtx.get(), kValidateIndex, &adaptor, &results, &output));
    ASSERT_EQUALS(true, results.valid);
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
    rs.reset(new WiredTigerRecordStore(opCtx.get(),
                                       "a.b",
                                       uri,
                                       kWiredTigerEngineName,
                                       false,
                                       false,
                                       -1,
                                       -1,
                                       NULL,
                                       sizeStorer.get()));
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


StatusWith<RecordId> insertBSON(ServiceContext::UniqueOperationContext& opCtx,
                                unique_ptr<RecordStore>& rs,
                                const Timestamp& opTime) {
    BSONObj obj = BSON("ts" << opTime);
    WriteUnitOfWork wuow(opCtx.get());
    WiredTigerRecordStore* wrs = checked_cast<WiredTigerRecordStore*>(rs.get());
    invariant(wrs);
    Status status = wrs->oplogDiskLocRegister(opCtx.get(), opTime);
    if (!status.isOK())
        return StatusWith<RecordId>(status);
    StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), false);
    if (res.isOK())
        wuow.commit();
    return res;
}

// TODO make generic
TEST(WiredTigerRecordStoreTest, OplogHack) {
    WiredTigerHarnessHelper harnessHelper;
    // Use a large enough cappedMaxSize so that the limit is not reached by doing the inserts within
    // the test itself.
    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    unique_ptr<RecordStore> rs(
        harnessHelper.newCappedRecordStore("local.oplog.foo", cappedMaxSize, -1));
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        // always illegal
        ASSERT_EQ(insertBSON(opCtx, rs, Timestamp(2, -1)).getStatus(), ErrorCodes::BadValue);

        {
            BSONObj obj = BSON("not_ts" << Timestamp(2, 1));
            ASSERT_EQ(
                rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), false).getStatus(),
                ErrorCodes::BadValue);

            obj = BSON("ts"
                       << "not a Timestamp");
            ASSERT_EQ(
                rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), false).getStatus(),
                ErrorCodes::BadValue);
        }

        // currently dasserts
        // ASSERT_EQ(insertBSON(opCtx, rs, BSON("ts" << Timestamp(-2,1))).getStatus(),
        // ErrorCodes::BadValue);

        // success cases
        ASSERT_EQ(insertBSON(opCtx, rs, Timestamp(1, 1)).getValue(), RecordId(1, 1));

        ASSERT_EQ(insertBSON(opCtx, rs, Timestamp(1, 2)).getValue(), RecordId(1, 2));

        ASSERT_EQ(insertBSON(opCtx, rs, Timestamp(2, 2)).getValue(), RecordId(2, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
        // find start
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(0, 1)), RecordId());      // nothing <=
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 1)), RecordId(1, 2));  // between
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 2)), RecordId(2, 2));  // ==
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId(2, 2));  // > highest
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
        rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(2, 2), false);  // no-op
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId(2, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
        rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(1, 2), false);  // deletes 2,2
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId(1, 2));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
        rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(1, 2), true);  // deletes 1,2
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId(1, 1));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->truncate(opCtx.get()));  // deletes 1,1 and leaves collection empty
        wuow.commit();
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId());
    }
}

TEST(WiredTigerRecordStoreTest, OplogHackOnNonOplog) {
    WiredTigerHarnessHelper harnessHelper;
    unique_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("local.NOT_oplog.foo"));

    ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

    BSONObj obj = BSON("ts" << Timestamp(2, -1));
    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(), false).getStatus());
        wuow.commit();
    }
    ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(0, 1)), boost::none);
}

TEST(WiredTigerRecordStoreTest, CappedOrder) {
    unique_ptr<WiredTigerHarnessHelper> harnessHelper(new WiredTigerHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newCappedRecordStore("a.b", 100000, 10000));

    RecordId id1;

    {  // first insert a document
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            id1 = res.getValue();
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekExact(id1);
        ASSERT_EQ(id1, record->id);
        ASSERT(!cursor->next());
    }

    {
        // now we insert 2 docs, but commit the 2nd one fiirst
        // we make sure we can't find the 2nd until the first is commited
        ServiceContext::UniqueOperationContext t1(harnessHelper->newOperationContext());
        unique_ptr<WriteUnitOfWork> w1(new WriteUnitOfWork(t1.get()));
        rs->insertRecord(t1.get(), "b", 2, false);
        // do not commit yet

        {  // create 2nd doc
            auto client2 = harnessHelper->serviceContext()->makeClient("c2");
            auto t2 = harnessHelper->newOperationContext(client2.get());
            {
                WriteUnitOfWork w2(t2.get());
                rs->insertRecord(t2.get(), "c", 2, false);
                w2.commit();
            }
        }

        {  // state should be the same
            auto client2 = harnessHelper->serviceContext()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            auto cursor = rs->getCursor(opCtx.get());
            auto record = cursor->seekExact(id1);
            ASSERT_EQ(id1, record->id);
            ASSERT(!cursor->next());
        }

        w1->commit();
    }

    {  // now all 3 docs should be visible
        auto client2 = harnessHelper->serviceContext()->makeClient("c2");
        auto opCtx = harnessHelper->newOperationContext(client2.get());
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekExact(id1);
        ASSERT_EQ(id1, record->id);
        ASSERT(cursor->next());
        ASSERT(cursor->next());
        ASSERT(!cursor->next());
    }
}

TEST(WiredTigerRecordStoreTest, CappedCursorRollover) {
    unique_ptr<WiredTigerHarnessHelper> harnessHelper(new WiredTigerHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newCappedRecordStore("a.b", 10000, 5));

    {  // first insert 3 documents
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        for (int i = 0; i < 3; ++i) {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            uow.commit();
        }
    }

    // set up our cursor that should rollover

    auto client2 = harnessHelper->serviceContext()->makeClient("c2");
    auto cursorCtx = harnessHelper->newOperationContext(client2.get());
    auto cursor = rs->getCursor(cursorCtx.get());
    ASSERT(cursor->next());
    cursor->save();
    cursorCtx->recoveryUnit()->abandonSnapshot();

    {  // insert 100 documents which causes rollover
        auto client3 = harnessHelper->serviceContext()->makeClient("c3");
        auto opCtx = harnessHelper->newOperationContext(client3.get());
        for (int i = 0; i < 100; i++) {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            uow.commit();
        }
    }

    // cursor should now be dead
    ASSERT_FALSE(cursor->restore());
    ASSERT(!cursor->next());
}

RecordId _oplogOrderInsertOplog(OperationContext* txn, unique_ptr<RecordStore>& rs, int inc) {
    Timestamp opTime = Timestamp(5, inc);
    WiredTigerRecordStore* wrs = checked_cast<WiredTigerRecordStore*>(rs.get());
    Status status = wrs->oplogDiskLocRegister(txn, opTime);
    ASSERT_OK(status);
    BSONObj obj = BSON("ts" << opTime);
    StatusWith<RecordId> res = rs->insertRecord(txn, obj.objdata(), obj.objsize(), false);
    ASSERT_OK(res.getStatus());
    return res.getValue();
}

TEST(WiredTigerRecordStoreTest, OplogOrder) {
    unique_ptr<WiredTigerHarnessHelper> harnessHelper(new WiredTigerHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newCappedRecordStore("local.oplog.foo", 100000, -1));

    {
        const WiredTigerRecordStore* wrs = checked_cast<WiredTigerRecordStore*>(rs.get());
        ASSERT(wrs->isOplog());
        ASSERT(wrs->usingOplogHack());
    }

    RecordId id1;

    {  // first insert a document
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            id1 = _oplogOrderInsertOplog(opCtx.get(), rs, 1);
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekExact(id1);
        ASSERT_EQ(id1, record->id);
        ASSERT(!cursor->next());
    }

    {
        // now we insert 2 docs, but commit the 2nd one first.
        // we make sure we can't find the 2nd until the first is commited.
        ServiceContext::UniqueOperationContext earlyReader(harnessHelper->newOperationContext());
        auto earlyCursor = rs->getCursor(earlyReader.get());
        ASSERT_EQ(earlyCursor->seekExact(id1)->id, id1);
        earlyCursor->save();
        earlyReader->recoveryUnit()->abandonSnapshot();

        auto client1 = harnessHelper->serviceContext()->makeClient("c1");
        auto t1 = harnessHelper->newOperationContext(client1.get());
        WriteUnitOfWork w1(t1.get());
        _oplogOrderInsertOplog(t1.get(), rs, 20);
        // do not commit yet

        {  // create 2nd doc
            auto client2 = harnessHelper->serviceContext()->makeClient("c2");
            auto t2 = harnessHelper->newOperationContext(client2.get());
            {
                WriteUnitOfWork w2(t2.get());
                _oplogOrderInsertOplog(t2.get(), rs, 30);
                w2.commit();
            }
        }

        {  // Other operations should not be able to see 2nd doc until w1 commits.
            earlyCursor->restore();
            ASSERT(!earlyCursor->next());

            auto client2 = harnessHelper->serviceContext()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            auto cursor = rs->getCursor(opCtx.get());
            auto record = cursor->seekExact(id1);
            ASSERT_EQ(id1, record->id);
            ASSERT(!cursor->next());
        }

        w1.commit();
    }

    {  // now all 3 docs should be visible
        auto client2 = harnessHelper->serviceContext()->makeClient("c2");
        auto opCtx = harnessHelper->newOperationContext(client2.get());
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekExact(id1);
        ASSERT_EQ(id1, record->id);
        ASSERT(cursor->next());
        ASSERT(cursor->next());
        ASSERT(!cursor->next());
    }

    // Rollback the last two oplog entries, then insert entries with older optimes and ensure that
    // the visibility rules aren't violated. See SERVER-21645
    {
        auto client2 = harnessHelper->serviceContext()->makeClient("c2");
        auto txn = harnessHelper->newOperationContext(client2.get());
        rs->temp_cappedTruncateAfter(txn.get(), id1, /*inclusive*/ false);
    }

    {
        // Now we insert 2 docs with timestamps earlier than before, but commit the 2nd one first.
        // We make sure we can't find the 2nd until the first is commited.
        ServiceContext::UniqueOperationContext earlyReader(harnessHelper->newOperationContext());
        auto earlyCursor = rs->getCursor(earlyReader.get());
        ASSERT_EQ(earlyCursor->seekExact(id1)->id, id1);
        earlyCursor->save();
        earlyReader->recoveryUnit()->abandonSnapshot();

        auto client1 = harnessHelper->serviceContext()->makeClient("c1");
        auto t1 = harnessHelper->newOperationContext(client1.get());
        WriteUnitOfWork w1(t1.get());
        _oplogOrderInsertOplog(t1.get(), rs, 2);
        // do not commit yet

        {  // create 2nd doc
            auto client2 = harnessHelper->serviceContext()->makeClient("c2");
            auto t2 = harnessHelper->newOperationContext(client2.get());
            {
                WriteUnitOfWork w2(t2.get());
                _oplogOrderInsertOplog(t2.get(), rs, 3);
                w2.commit();
            }
        }

        {  // Other operations should not be able to see 2nd doc until w1 commits.
            ASSERT(earlyCursor->restore());
            ASSERT(!earlyCursor->next());

            auto client2 = harnessHelper->serviceContext()->makeClient("c2");
            auto opCtx = harnessHelper->newOperationContext(client2.get());
            auto cursor = rs->getCursor(opCtx.get());
            auto record = cursor->seekExact(id1);
            ASSERT_EQ(id1, record->id);
            ASSERT(!cursor->next());
        }

        w1.commit();
    }

    {  // now all 3 docs should be visible
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekExact(id1);
        ASSERT_EQ(id1, record->id);
        ASSERT(cursor->next());
        ASSERT(cursor->next());
        ASSERT(!cursor->next());
    }
}

TEST(WiredTigerRecordStoreTest, StorageSizeStatisticsDisabled) {
    WiredTigerHarnessHelper harnessHelper("statistics=(none)");
    unique_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("a.b"));

    ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
    ASSERT_THROWS(rs->storageSize(opCtx.get()), UserException);
}

TEST(WiredTigerRecordStoreTest, AppendCustomStatsMetadata) {
    WiredTigerHarnessHelper harnessHelper;
    unique_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("a.b"));

    ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
    BSONObjBuilder builder;
    rs->appendCustomStats(opCtx.get(), &builder, 1.0);
    BSONObj customStats = builder.obj();

    BSONElement wiredTigerElement = customStats.getField(kWiredTigerEngineName);
    ASSERT_TRUE(wiredTigerElement.isABSONObj());
    BSONObj wiredTiger = wiredTigerElement.Obj();

    BSONElement metadataElement = wiredTiger.getField("metadata");
    ASSERT_TRUE(metadataElement.isABSONObj());
    BSONObj metadata = metadataElement.Obj();

    BSONElement versionElement = metadata.getField("formatVersion");
    ASSERT_TRUE(versionElement.isNumber());

    BSONElement creationStringElement = wiredTiger.getField("creationString");
    ASSERT_EQUALS(creationStringElement.type(), String);
}

TEST(WiredTigerRecordStoreTest, CappedCursorYieldFirst) {
    unique_ptr<WiredTigerHarnessHelper> harnessHelper(new WiredTigerHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newCappedRecordStore("a.b", 10000, 50));

    RecordId id1;

    {  // first insert a document
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, false);
        ASSERT_OK(res.getStatus());
        id1 = res.getValue();
        uow.commit();
    }

    ServiceContext::UniqueOperationContext cursorCtx(harnessHelper->newOperationContext());
    auto cursor = rs->getCursor(cursorCtx.get());

    // See that things work if you yield before you first call next().
    cursor->save();
    cursorCtx->recoveryUnit()->abandonSnapshot();
    ASSERT_TRUE(cursor->restore());
    auto record = cursor->next();
    ASSERT(record);
    ASSERT_EQ(id1, record->id);
    ASSERT(!cursor->next());
}

BSONObj makeBSONObjWithSize(const Timestamp& opTime, int size, char fill = 'x') {
    BSONObj objTemplate = BSON("ts" << opTime << "str"
                                    << "");
    ASSERT_LTE(objTemplate.objsize(), size);
    std::string str(size - objTemplate.objsize(), fill);

    BSONObj obj = BSON("ts" << opTime << "str" << str);
    ASSERT_EQ(size, obj.objsize());

    return obj;
}

StatusWith<RecordId> insertBSONWithSize(OperationContext* opCtx,
                                        RecordStore* rs,
                                        const Timestamp& opTime,
                                        int size) {
    BSONObj obj = makeBSONObjWithSize(opTime, size);

    WriteUnitOfWork wuow(opCtx);
    WiredTigerRecordStore* wtrs = checked_cast<WiredTigerRecordStore*>(rs);
    invariant(wtrs);
    Status status = wtrs->oplogDiskLocRegister(opCtx, opTime);
    if (!status.isOK()) {
        return StatusWith<RecordId>(status);
    }
    StatusWith<RecordId> res = rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), false);
    if (res.isOK()) {
        wuow.commit();
    }
    return res;
}

// Insert records into an oplog and verify the number of stones that are created.
TEST(WiredTigerRecordStoreTest, OplogStones_CreateNewStone) {
    WiredTigerHarnessHelper harnessHelper;

    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    unique_ptr<RecordStore> rs(
        harnessHelper.newCappedRecordStore("local.oplog.stones", cappedMaxSize, -1));

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        ASSERT_EQ(0U, oplogStones->numStones());

        // Inserting a record smaller than 'minBytesPerStone' shouldn't create a new oplog stone.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 99), RecordId(1, 1));
        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(99, oplogStones->currentBytes());

        // Inserting another record such that their combined size exceeds 'minBytesPerStone' should
        // cause a new stone to be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 51), RecordId(1, 2));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());

        // Inserting a record such that the combined size of this record and the previously inserted
        // one exceed 'minBytesPerStone' shouldn't cause a new stone to be created because we've
        // started filling a new stone.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 3), 50), RecordId(1, 3));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());

        // Inserting a record such that the combined size of this record and the previously inserted
        // one is exactly equal to 'minBytesPerStone' should cause a new stone to be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 4), 50), RecordId(1, 4));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());

        // Inserting a single record that exceeds 'minBytesPerStone' should cause a new stone to
        // be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 5), 101), RecordId(1, 5));
        ASSERT_EQ(3U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }
}

// Insert records into an oplog and try to update them. The updates shouldn't succeed if the size of
// record is changed.
TEST(WiredTigerRecordStoreTest, OplogStones_UpdateRecord) {
    WiredTigerHarnessHelper harnessHelper;

    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    unique_ptr<RecordStore> rs(
        harnessHelper.newCappedRecordStore("local.oplog.stones", cappedMaxSize, -1));

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);

    // Insert two records such that one makes up a full stone and the other is a part of the stone
    // currently being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 100), RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 50), RecordId(1, 2));

        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    // Attempts to grow the records should fail.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        BSONObj changed1 = makeBSONObjWithSize(Timestamp(1, 1), 101);
        BSONObj changed2 = makeBSONObjWithSize(Timestamp(1, 2), 51);

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_NOT_OK(rs->updateRecord(
            opCtx.get(), RecordId(1, 1), changed1.objdata(), changed1.objsize(), false, nullptr));
        ASSERT_NOT_OK(rs->updateRecord(
            opCtx.get(), RecordId(1, 2), changed2.objdata(), changed2.objsize(), false, nullptr));
    }

    // Attempts to shrink the records should also fail.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        BSONObj changed1 = makeBSONObjWithSize(Timestamp(1, 1), 99);
        BSONObj changed2 = makeBSONObjWithSize(Timestamp(1, 2), 49);

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_NOT_OK(rs->updateRecord(
            opCtx.get(), RecordId(1, 1), changed1.objdata(), changed1.objsize(), false, nullptr));
        ASSERT_NOT_OK(rs->updateRecord(
            opCtx.get(), RecordId(1, 2), changed2.objdata(), changed2.objsize(), false, nullptr));
    }

    // Changing the contents of the records without changing their size should succeed.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        BSONObj changed1 = makeBSONObjWithSize(Timestamp(1, 1), 100, 'y');
        BSONObj changed2 = makeBSONObjWithSize(Timestamp(1, 2), 50, 'z');

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->updateRecord(
            opCtx.get(), RecordId(1, 1), changed1.objdata(), changed1.objsize(), false, nullptr));
        ASSERT_OK(rs->updateRecord(
            opCtx.get(), RecordId(1, 2), changed2.objdata(), changed2.objsize(), false, nullptr));
        wuow.commit();

        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }
}

// Insert multiple records and truncate the oplog using RecordStore::truncate(). The operation
// should leave no stones, including the partially filled one.
TEST(WiredTigerRecordStoreTest, OplogStones_Truncate) {
    WiredTigerHarnessHelper harnessHelper;

    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    unique_ptr<RecordStore> rs(
        harnessHelper.newCappedRecordStore("local.oplog.stones", cappedMaxSize, -1));

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 50), RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 50), RecordId(1, 2));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 3), 50), RecordId(1, 3));

        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(150, rs->dataSize(opCtx.get()));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->truncate(opCtx.get()));
        wuow.commit();

        ASSERT_EQ(0, rs->dataSize(opCtx.get()));
        ASSERT_EQ(0, rs->numRecords(opCtx.get()));
        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }
}

// Insert multiple records, truncate the oplog using RecordStore::temp_cappedTruncateAfter(), and
// verify that the metadata for each stone is updated. If a full stone is partially truncated, then
// it should become the stone currently being filled.
TEST(WiredTigerRecordStoreTest, OplogStones_CappedTruncateAfter) {
    WiredTigerHarnessHelper harnessHelper;

    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    unique_ptr<RecordStore> rs(
        harnessHelper.newCappedRecordStore("local.oplog.stones", cappedMaxSize, -1));

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(1000);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 400), RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 800), RecordId(1, 2));

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 3), 200), RecordId(1, 3));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 4), 250), RecordId(1, 4));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 5), 300), RecordId(1, 5));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 6), 350), RecordId(1, 6));

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 7), 50), RecordId(1, 7));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 8), 100), RecordId(1, 8));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 9), 150), RecordId(1, 9));

        ASSERT_EQ(9, rs->numRecords(opCtx.get()));
        ASSERT_EQ(2600, rs->dataSize(opCtx.get()));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(3, oplogStones->currentRecords());
        ASSERT_EQ(300, oplogStones->currentBytes());
    }

    // Truncate data using an inclusive RecordId that exists inside the stone currently being
    // filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(1, 8), true);

        ASSERT_EQ(7, rs->numRecords(opCtx.get()));
        ASSERT_EQ(2350, rs->dataSize(opCtx.get()));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    // Truncate data using an inclusive RecordId that refers to the 'lastRecord' of a full stone.
    // The stone should become the one currently being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(1, 6), true);

        ASSERT_EQ(5, rs->numRecords(opCtx.get()));
        ASSERT_EQ(1950, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(3, oplogStones->currentRecords());
        ASSERT_EQ(750, oplogStones->currentBytes());
    }

    // Truncate data using a non-inclusive RecordId that exists inside the stone currently being
    // filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(1, 3), false);

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(1400, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(200, oplogStones->currentBytes());
    }

    // Truncate data using a non-inclusive RecordId that refers to the 'lastRecord' of a full stone.
    // The stone should remain intact.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(1, 2), false);

        ASSERT_EQ(2, rs->numRecords(opCtx.get()));
        ASSERT_EQ(1200, rs->dataSize(opCtx.get()));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    // Truncate data using a non-inclusive RecordId that exists inside a full stone. The stone
    // should become the one currently being filled.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(1, 1), false);

        ASSERT_EQ(1, rs->numRecords(opCtx.get()));
        ASSERT_EQ(400, rs->dataSize(opCtx.get()));
        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(400, oplogStones->currentBytes());
    }
}

// Verify that oplog stones are reclaimed when the number of stones to keep is exceeded.
TEST(WiredTigerRecordStoreTest, OplogStones_ReclaimStones) {
    WiredTigerHarnessHelper harnessHelper;

    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    unique_ptr<RecordStore> rs(
        harnessHelper.newCappedRecordStore("local.oplog.stones", cappedMaxSize, -1));

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);
    oplogStones->setNumStonesToKeep(2U);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 100), RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 110), RecordId(1, 2));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 3), 120), RecordId(1, 3));

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(330, rs->dataSize(opCtx.get()));
        ASSERT_EQ(3U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    // Truncate a stone when number of stones to keep is exceeded.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        wtrs->reclaimOplog(opCtx.get());

        ASSERT_EQ(2, rs->numRecords(opCtx.get()));
        ASSERT_EQ(230, rs->dataSize(opCtx.get()));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 4), 130), RecordId(1, 4));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 5), 140), RecordId(1, 5));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 6), 50), RecordId(1, 6));

        ASSERT_EQ(5, rs->numRecords(opCtx.get()));
        ASSERT_EQ(550, rs->dataSize(opCtx.get()));
        ASSERT_EQ(4U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    // Truncate multiple stones if necessary.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        wtrs->reclaimOplog(opCtx.get());

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(320, rs->dataSize(opCtx.get()));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }

    // No-op if the number of oplog stones is less than or equal to the number of stones to keep.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        wtrs->reclaimOplog(opCtx.get());

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(320, rs->dataSize(opCtx.get()));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());
    }
}

// Verify that oplog stones are not reclaimed even if the size of the record store exceeds
// 'cappedMaxSize'.
TEST(WiredTigerRecordStoreTest, OplogStones_ExceedCappedMaxSize) {
    WiredTigerHarnessHelper harnessHelper;

    const int64_t cappedMaxSize = 256;
    unique_ptr<RecordStore> rs(
        harnessHelper.newCappedRecordStore("local.oplog.stones", cappedMaxSize, -1));

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);
    oplogStones->setNumStonesToKeep(10U);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 100), RecordId(1, 1));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 2), 110), RecordId(1, 2));
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 3), 120), RecordId(1, 3));

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(330, rs->dataSize(opCtx.get()));
        ASSERT_EQ(3U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }

    // Shouldn't truncate a stone when the number of oplog stones is less than the number of stones
    // to keep, even though the size of the record store exceeds 'cappedMaxSize'.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        wtrs->reclaimOplog(opCtx.get());

        ASSERT_EQ(3, rs->numRecords(opCtx.get()));
        ASSERT_EQ(330, rs->dataSize(opCtx.get()));
        ASSERT_EQ(3U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }
}

// Verify that an oplog stone isn't created if it would cause the logical representation of the
// records to not be in increasing order.
TEST(WiredTigerRecordStoreTest, OplogStones_AscendingOrder) {
    WiredTigerHarnessHelper harnessHelper;

    const int64_t cappedMaxSize = 10 * 1024;  // 10KB
    unique_ptr<RecordStore> rs(
        harnessHelper.newCappedRecordStore("local.oplog.stones", cappedMaxSize, -1));

    WiredTigerRecordStore* wtrs = static_cast<WiredTigerRecordStore*>(rs.get());
    WiredTigerRecordStore::OplogStones* oplogStones = wtrs->oplogStones();

    oplogStones->setMinBytesPerStone(100);

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(2, 2), 50), RecordId(2, 2));
        ASSERT_EQ(0U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(50, oplogStones->currentBytes());

        // Inserting a record that has a smaller RecordId than the previously inserted record should
        // be able to create a new stone when no stones already exist.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(2, 1), 50), RecordId(2, 1));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());

        // However, inserting a record that has a smaller RecordId than most recently created
        // stone's last record shouldn't cause a new stone to be created, even if the size of the
        // inserted record exceeds 'minBytesPerStone'.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(1, 1), 100), RecordId(1, 1));
        ASSERT_EQ(1U, oplogStones->numStones());
        ASSERT_EQ(1, oplogStones->currentRecords());
        ASSERT_EQ(100, oplogStones->currentBytes());

        // Inserting a record that has a larger RecordId than the most recently created stone's last
        // record should then cause a new stone to be created.
        ASSERT_EQ(insertBSONWithSize(opCtx.get(), rs.get(), Timestamp(2, 3), 50), RecordId(2, 3));
        ASSERT_EQ(2U, oplogStones->numStones());
        ASSERT_EQ(0, oplogStones->currentRecords());
        ASSERT_EQ(0, oplogStones->currentBytes());
    }
}

}  // namespace mongo
