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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/base/checked_cast.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::unique_ptr;
using std::string;
using std::stringstream;

class WiredTigerHarnessHelper : public HarnessHelper {
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

    virtual RecordStore* newNonCappedRecordStore() {
        return newNonCappedRecordStore("a.b");
    }
    RecordStore* newNonCappedRecordStore(const std::string& ns) {
        WiredTigerRecoveryUnit* ru = new WiredTigerRecoveryUnit(_sessionCache);
        OperationContextNoop txn(ru);
        string uri = "table:" + ns;

        StatusWith<std::string> result =
            WiredTigerRecordStore::generateCreateString(ns, CollectionOptions(), "");
        ASSERT_TRUE(result.isOK());
        std::string config = result.getValue();

        {
            WriteUnitOfWork uow(&txn);
            WT_SESSION* s = ru->getSession(&txn)->getSession();
            invariantWTOK(s->create(s, uri.c_str(), config.c_str()));
            uow.commit();
        }

        return new WiredTigerRecordStore(&txn, ns, uri);
    }

    virtual RecordStore* newCappedRecordStore(const std::string& ns,
                                              int64_t cappedMaxSize,
                                              int64_t cappedMaxDocs) {
        WiredTigerRecoveryUnit* ru = new WiredTigerRecoveryUnit(_sessionCache);
        OperationContextNoop txn(ru);
        string uri = "table:a.b";

        CollectionOptions options;
        options.capped = true;

        StatusWith<std::string> result =
            WiredTigerRecordStore::generateCreateString(ns, options, "");
        ASSERT_TRUE(result.isOK());
        std::string config = result.getValue();

        {
            WriteUnitOfWork uow(&txn);
            WT_SESSION* s = ru->getSession(&txn)->getSession();
            invariantWTOK(s->create(s, uri.c_str(), config.c_str()));
            uow.commit();
        }

        return new WiredTigerRecordStore(&txn, ns, uri, true, cappedMaxSize, cappedMaxDocs);
    }

    virtual RecoveryUnit* newRecoveryUnit() {
        return new WiredTigerRecoveryUnit(_sessionCache);
    }

    WT_CONNECTION* conn() const {
        return _conn;
    }

private:
    unittest::TempDir _dbpath;
    WT_CONNECTION* _conn;
    WiredTigerSessionCache* _sessionCache;
};

HarnessHelper* newHarnessHelper() {
    return new WiredTigerHarnessHelper();
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

TEST(WiredTigerRecordStoreTest, GenerateCreateStringValidConfigFormat) {
    // TODO eventually this should fail since "abc" is not a valid WT option.
    BSONObj spec = fromjson("{configString: 'abc=def'}");
    StatusWith<std::string> result = WiredTigerRecordStore::parseOptionsField(spec);
    const Status& status = result.getStatus();
    ASSERT_OK(status);
    ASSERT_EQ(result.getValue(), "abc=def,");
}

TEST(WiredTigerRecordStoreTest, Isolation1) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    RecordId loc1;
    RecordId loc2;

    {
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());

            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            loc1 = res.getValue();

            res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            loc2 = res.getValue();

            uow.commit();
        }
    }

    {
        unique_ptr<OperationContext> t1(harnessHelper->newOperationContext());
        unique_ptr<OperationContext> t2(harnessHelper->newOperationContext());

        unique_ptr<WriteUnitOfWork> w1(new WriteUnitOfWork(t1.get()));
        unique_ptr<WriteUnitOfWork> w2(new WriteUnitOfWork(t2.get()));

        rs->dataFor(t1.get(), loc1);
        rs->dataFor(t2.get(), loc1);

        ASSERT_OK(rs->updateRecord(t1.get(), loc1, "b", 2, false, NULL).getStatus());
        ASSERT_OK(rs->updateRecord(t1.get(), loc2, "B", 2, false, NULL).getStatus());

        try {
            // this should fail
            rs->updateRecord(t2.get(), loc1, "c", 2, false, NULL);
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

    RecordId loc1;
    RecordId loc2;

    {
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());

            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            loc1 = res.getValue();

            res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            loc2 = res.getValue();

            uow.commit();
        }
    }

    {
        unique_ptr<OperationContext> t1(harnessHelper->newOperationContext());
        unique_ptr<OperationContext> t2(harnessHelper->newOperationContext());

        // ensure we start transactions
        rs->dataFor(t1.get(), loc2);
        rs->dataFor(t2.get(), loc2);

        {
            WriteUnitOfWork w(t1.get());
            ASSERT_OK(rs->updateRecord(t1.get(), loc1, "b", 2, false, NULL).getStatus());
            w.commit();
        }

        {
            WriteUnitOfWork w(t2.get());
            ASSERT_EQUALS(string("a"), rs->dataFor(t2.get(), loc1).data());
            try {
                // this should fail as our version of loc1 is too old
                rs->updateRecord(t2.get(), loc1, "c", 2, false, NULL);
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
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
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
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
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
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        rs.reset(new WiredTigerRecordStore(opCtx.get(), "a.b", uri, false, -1, -1, NULL, &ss));
    }

    {
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(N, rs->numRecords(opCtx.get()));
    }

    {
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
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
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
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
    virtual Status validate(const RecordData& record, size_t* dataSize) {
        *dataSize = static_cast<size_t>(record.size());
        return Status::OK();
    }
};

class BadValidateAdaptor : public ValidateAdaptor {
public:
    virtual Status validate(const RecordData& record, size_t* dataSize) {
        *dataSize = static_cast<size_t>(record.size());
        return Status(ErrorCodes::UnknownError, "");
    }
};

class SizeStorerValidateTest : public mongo::unittest::Test {
private:
    virtual void setUp() {
        harnessHelper.reset(new WiredTigerHarnessHelper());
        sizeStorer.reset(new WiredTigerSizeStorer(harnessHelper->conn(), "table:sizeStorer"));
        rs.reset(harnessHelper->newNonCappedRecordStore());
        WiredTigerRecordStore* wtrs = checked_cast<WiredTigerRecordStore*>(rs.get());
        wtrs->setSizeStorer(sizeStorer.get());
        uri = wtrs->getURI();

        expectedNumRecords = 10000;
        expectedDataSize = expectedNumRecords * 2;
        {
            unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
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

// Basic validation - size storer data is not updated.
TEST_F(SizeStorerValidateTest, Basic) {
    unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
    ValidateResults results;
    BSONObjBuilder output;
    ASSERT_OK(rs->validate(opCtx.get(), false, false, NULL, &results, &output));
    BSONObj obj = output.obj();
    ASSERT_EQUALS(expectedNumRecords, obj.getIntField("nrecords"));
    ASSERT_EQUALS(0, getNumRecords());
    ASSERT_EQUALS(0, getDataSize());
}

// Full validation - size storer data is updated.
TEST_F(SizeStorerValidateTest, FullWithGoodAdaptor) {
    unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
    GoodValidateAdaptor adaptor;
    ValidateResults results;
    BSONObjBuilder output;
    ASSERT_OK(rs->validate(opCtx.get(), true, true, &adaptor, &results, &output));
    BSONObj obj = output.obj();
    ASSERT_EQUALS(expectedNumRecords, obj.getIntField("nrecords"));
    ASSERT_EQUALS(expectedNumRecords, getNumRecords());
    ASSERT_EQUALS(expectedDataSize, getDataSize());
}

// Full validation with a validation adaptor that fails - size storer data is not updated.
TEST_F(SizeStorerValidateTest, FullWithBadAdapter) {
    unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
    BadValidateAdaptor adaptor;
    ValidateResults results;
    BSONObjBuilder output;
    ASSERT_OK(rs->validate(opCtx.get(), true, true, &adaptor, &results, &output));
    BSONObj obj = output.obj();
    ASSERT_EQUALS(expectedNumRecords, obj.getIntField("nrecords"));
    ASSERT_EQUALS(0, getNumRecords());
    ASSERT_EQUALS(0, getDataSize());
}

// Load bad _numRecords and _dataSize values at record store creation.
TEST_F(SizeStorerValidateTest, InvalidSizeStorerAtCreation) {
    rs.reset(NULL);

    unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
    sizeStorer->storeToCache(uri, expectedNumRecords * 2, expectedDataSize * 2);
    rs.reset(
        new WiredTigerRecordStore(opCtx.get(), "a.b", uri, false, -1, -1, NULL, sizeStorer.get()));
    ASSERT_EQUALS(expectedNumRecords * 2, rs->numRecords(NULL));
    ASSERT_EQUALS(expectedDataSize * 2, rs->dataSize(NULL));

    // Full validation should fix record and size counters.
    GoodValidateAdaptor adaptor;
    ValidateResults results;
    BSONObjBuilder output;
    ASSERT_OK(rs->validate(opCtx.get(), true, true, &adaptor, &results, &output));
    BSONObj obj = output.obj();
    ASSERT_EQUALS(expectedNumRecords, obj.getIntField("nrecords"));
    ASSERT_EQUALS(expectedNumRecords, getNumRecords());
    ASSERT_EQUALS(expectedDataSize, getDataSize());

    ASSERT_EQUALS(expectedNumRecords, rs->numRecords(NULL));
    ASSERT_EQUALS(expectedDataSize, rs->dataSize(NULL));
}

}  // namespace


StatusWith<RecordId> insertBSON(unique_ptr<OperationContext>& opCtx,
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
    unique_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("local.oplog.foo"));
    {
        unique_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());

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
        unique_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
        // find start
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(0, 1)), RecordId());      // nothing <=
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 1)), RecordId(1, 2));  // between
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 2)), RecordId(2, 2));  // ==
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId(2, 2));  // > highest
    }

    {
        unique_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
        rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(2, 2), false);  // no-op
    }

    {
        unique_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId(2, 2));
    }

    {
        unique_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
        rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(1, 2), false);  // deletes 2,2
    }

    {
        unique_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId(1, 2));
    }

    {
        unique_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
        rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(1, 2), true);  // deletes 1,2
    }

    {
        unique_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId(1, 1));
    }

    {
        unique_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->truncate(opCtx.get()));  // deletes 1,1 and leaves collection empty
        wuow.commit();
    }

    {
        unique_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2, 3)), RecordId());
    }
}

TEST(WiredTigerRecordStoreTest, OplogHackOnNonOplog) {
    WiredTigerHarnessHelper harnessHelper;
    unique_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("local.NOT_oplog.foo"));

    unique_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());

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

    RecordId loc1;

    {  // first insert a document
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            loc1 = res.getValue();
            uow.commit();
        }
    }

    {
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekExact(loc1);
        ASSERT_EQ(loc1, record->id);
        ASSERT(!cursor->next());
    }

    {
        // now we insert 2 docs, but commit the 2nd one fiirst
        // we make sure we can't find the 2nd until the first is commited
        unique_ptr<OperationContext> t1(harnessHelper->newOperationContext());
        unique_ptr<WriteUnitOfWork> w1(new WriteUnitOfWork(t1.get()));
        rs->insertRecord(t1.get(), "b", 2, false);
        // do not commit yet

        {  // create 2nd doc
            unique_ptr<OperationContext> t2(harnessHelper->newOperationContext());
            {
                WriteUnitOfWork w2(t2.get());
                rs->insertRecord(t2.get(), "c", 2, false);
                w2.commit();
            }
        }

        {  // state should be the same
            unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
            auto cursor = rs->getCursor(opCtx.get());
            auto record = cursor->seekExact(loc1);
            ASSERT_EQ(loc1, record->id);
            ASSERT(!cursor->next());
        }

        w1->commit();
    }

    {  // now all 3 docs should be visible
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekExact(loc1);
        ASSERT_EQ(loc1, record->id);
        ASSERT(cursor->next());
        ASSERT(cursor->next());
        ASSERT(!cursor->next());
    }
}

TEST(WiredTigerRecordStoreTest, CappedCursorRollover) {
    unique_ptr<WiredTigerHarnessHelper> harnessHelper(new WiredTigerHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newCappedRecordStore("a.b", 10000, 5));

    {  // first insert 3 documents
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        for (int i = 0; i < 3; ++i) {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            uow.commit();
        }
    }

    // set up our cursor that should rollover
    unique_ptr<OperationContext> cursorCtx(harnessHelper->newOperationContext());
    auto cursor = rs->getCursor(cursorCtx.get());
    ASSERT(cursor->next());
    cursor->savePositioned();
    cursorCtx->recoveryUnit()->abandonSnapshot();

    {  // insert 100 documents which causes rollover
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        for (int i = 0; i < 100; i++) {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, false);
            ASSERT_OK(res.getStatus());
            uow.commit();
        }
    }

    // cursor should now be dead
    ASSERT_FALSE(cursor->restore(cursorCtx.get()));
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

    RecordId loc1;

    {  // first insert a document
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            loc1 = _oplogOrderInsertOplog(opCtx.get(), rs, 1);
            uow.commit();
        }
    }

    {
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekExact(loc1);
        ASSERT_EQ(loc1, record->id);
        ASSERT(!cursor->next());
    }

    {
        // now we insert 2 docs, but commit the 2nd one fiirst
        // we make sure we can't find the 2nd until the first is commited
        unique_ptr<OperationContext> t1(harnessHelper->newOperationContext());
        unique_ptr<WriteUnitOfWork> w1(new WriteUnitOfWork(t1.get()));
        _oplogOrderInsertOplog(t1.get(), rs, 2);
        // do not commit yet

        {  // create 2nd doc
            unique_ptr<OperationContext> t2(harnessHelper->newOperationContext());
            {
                WriteUnitOfWork w2(t2.get());
                _oplogOrderInsertOplog(t2.get(), rs, 3);
                w2.commit();
            }
        }

        {  // state should be the same
            unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
            auto cursor = rs->getCursor(opCtx.get());
            auto record = cursor->seekExact(loc1);
            ASSERT_EQ(loc1, record->id);
            ASSERT(!cursor->next());
        }

        w1->commit();
    }

    {  // now all 3 docs should be visible
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get());
        auto record = cursor->seekExact(loc1);
        ASSERT_EQ(loc1, record->id);
        ASSERT(cursor->next());
        ASSERT(cursor->next());
        ASSERT(!cursor->next());
    }
}

TEST(WiredTigerRecordStoreTest, StorageSizeStatisticsDisabled) {
    WiredTigerHarnessHelper harnessHelper("statistics=(none)");
    unique_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("a.b"));

    unique_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
    ASSERT_THROWS(rs->storageSize(opCtx.get()), UserException);
}

TEST(WiredTigerRecordStoreTest, AppendCustomStatsMetadata) {
    WiredTigerHarnessHelper harnessHelper;
    unique_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("a.b"));

    unique_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
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

    RecordId loc1;

    {  // first insert a document
        unique_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "a", 2, false);
        ASSERT_OK(res.getStatus());
        loc1 = res.getValue();
        uow.commit();
    }

    unique_ptr<OperationContext> cursorCtx(harnessHelper->newOperationContext());
    auto cursor = rs->getCursor(cursorCtx.get());

    // See that things work if you yield before you first call getNext().
    cursor->savePositioned();
    cursorCtx->recoveryUnit()->abandonSnapshot();
    ASSERT_TRUE(cursor->restore(cursorCtx.get()));
    auto record = cursor->next();
    ASSERT_EQ(loc1, record->id);
    ASSERT(!cursor->next());
}

}  // namespace mongo
