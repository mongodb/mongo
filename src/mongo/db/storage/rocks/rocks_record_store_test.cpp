// rocks_record_store_harness_test.cpp

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

#include <memory>
#include <vector>

#include <boost/filesystem/operations.hpp>

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/db/storage/rocks/rocks_transaction.h"
#include "mongo/unittest/unittest.h"
#include "mongo/unittest/temp_dir.h"

namespace mongo {

    class RocksRecordStoreHarnessHelper : public HarnessHelper {
    public:
        RocksRecordStoreHarnessHelper() : _tempDir(_testNamespace) {
            boost::filesystem::remove_all(_tempDir.path());
            rocksdb::DB* db;
            std::vector<rocksdb::ColumnFamilyDescriptor> cfs;
            cfs.emplace_back();
            cfs.emplace_back("record_store", rocksdb::ColumnFamilyOptions());
            cfs[1].options.comparator = RocksRecordStore::newRocksCollectionComparator();
            rocksdb::DBOptions db_options;
            db_options.create_if_missing = true;
            db_options.create_missing_column_families = true;
            std::vector<rocksdb::ColumnFamilyHandle*> handles;
            auto s = rocksdb::DB::Open(db_options, _tempDir.path(), cfs, &handles, &db);
            ASSERT(s.ok());
            _db.reset(db);
            delete handles[0];
            _cf.reset(handles[1]);
        }

        virtual RecordStore* newNonCappedRecordStore() {
            return new RocksRecordStore("foo.bar", "1", _db.get(), _cf);
        }

        virtual RecoveryUnit* newRecoveryUnit() {
            return new RocksRecoveryUnit(&_transactionEngine, _db.get(), true);
        }

    private:
        string _testNamespace = "mongo-rocks-record-store-test";
        unittest::TempDir _tempDir;
        boost::scoped_ptr<rocksdb::DB> _db;
        boost::shared_ptr<rocksdb::ColumnFamilyHandle> _cf;
        RocksTransactionEngine _transactionEngine;
    };

    HarnessHelper* newHarnessHelper() { return new RocksRecordStoreHarnessHelper(); }

    TEST(RocksRecordStoreTest, Isolation1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        RecordId loc1;
        RecordId loc2;

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );

                StatusWith<RecordId> res = rs->insertRecord( opCtx.get(), "a", 2, false );
                ASSERT_OK( res.getStatus() );
                loc1 = res.getValue();

                res = rs->insertRecord( opCtx.get(), "a", 2, false );
                ASSERT_OK( res.getStatus() );
                loc2 = res.getValue();

                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> t1( harnessHelper->newOperationContext() );
            scoped_ptr<OperationContext> t2( harnessHelper->newOperationContext() );

            scoped_ptr<WriteUnitOfWork> w1( new WriteUnitOfWork( t1.get() ) );
            scoped_ptr<WriteUnitOfWork> w2( new WriteUnitOfWork( t2.get() ) );

            rs->dataFor( t1.get(), loc1 );
            rs->dataFor( t2.get(), loc1 );

            ASSERT_OK( rs->updateRecord( t1.get(), loc1, "b", 2, false, NULL ).getStatus() );
            ASSERT_OK( rs->updateRecord( t1.get(), loc2, "B", 2, false, NULL ).getStatus() );

            try {
                // this should fail
                rs->updateRecord( t2.get(), loc1, "c", 2, false, NULL );
                ASSERT( 0 );
            }
            catch ( WriteConflictException& dle ) {
                w2.reset( NULL );
                t2.reset( NULL );
            }

            w1->commit(); // this should succeed
        }
    }

    TEST(RocksRecordStoreTest, Isolation2 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        RecordId loc1;
        RecordId loc2;

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );

                StatusWith<RecordId> res = rs->insertRecord( opCtx.get(), "a", 2, false );
                ASSERT_OK( res.getStatus() );
                loc1 = res.getValue();

                res = rs->insertRecord( opCtx.get(), "a", 2, false );
                ASSERT_OK( res.getStatus() );
                loc2 = res.getValue();

                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> t1( harnessHelper->newOperationContext() );
            scoped_ptr<OperationContext> t2( harnessHelper->newOperationContext() );

            // ensure we start transactions
            rs->dataFor( t1.get(), loc2 );
            rs->dataFor( t2.get(), loc2 );

            {
                WriteUnitOfWork w( t1.get() );
                ASSERT_OK( rs->updateRecord( t1.get(), loc1, "b", 2, false, NULL ).getStatus() );
                w.commit();
            }

            {
                WriteUnitOfWork w( t2.get() );
                ASSERT_EQUALS( string("a"), rs->dataFor( t2.get(), loc1 ).data() );
                try {
                    // this should fail as our version of loc1 is too old
                    rs->updateRecord( t2.get(), loc1, "c", 2, false, NULL );
                    ASSERT( 0 );
                }
                catch ( WriteConflictException& dle ) {
                }

            }

        }
    }
}
