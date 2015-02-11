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

#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/filesystem/operations.hpp>
#include <string>

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/db/storage/rocks/rocks_engine.h"
#include "mongo/db/storage/rocks/rocks_index.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/db/storage/rocks/rocks_transaction.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    using boost::scoped_ptr;
    using boost::shared_ptr;
    using std::string;

    class RocksIndexHarness : public HarnessHelper {
    public:
        RocksIndexHarness() : _order(Ordering::make(BSONObj())), _tempDir(_testNamespace) {
            boost::filesystem::remove_all(_tempDir.path());
            rocksdb::DB* db;
            rocksdb::Options options;
            options.create_if_missing = true;
            auto s = rocksdb::DB::Open(options, _tempDir.path(), &db);
            ASSERT(s.ok());
            _db.reset(db);
        }

        virtual SortedDataInterface* newSortedDataInterface(bool unique) {
            if (unique) {
                return new RocksUniqueIndex(_db.get(), "prefix", "ident", _order);
            } else {
                return new RocksStandardIndex(_db.get(), "prefix", "ident", _order);
            }
        }

        virtual RecoveryUnit* newRecoveryUnit() {
            return new RocksRecoveryUnit(&_transactionEngine, _db.get(), true);
        }

    private:
        Ordering _order;
        string _testNamespace = "mongo-rocks-sorted-data-test";
        unittest::TempDir _tempDir;
        scoped_ptr<rocksdb::DB> _db;
        RocksTransactionEngine _transactionEngine;
    };

    HarnessHelper* newHarnessHelper() { return new RocksIndexHarness(); }

    TEST(RocksIndexTest, Isolation) {
        scoped_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
        scoped_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(true));

        {
            scoped_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
            ASSERT(sorted->isEmpty(opCtx.get()));
        }

        {
            scoped_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
            {
                WriteUnitOfWork uow(opCtx.get());

                ASSERT_OK(sorted->insert(opCtx.get(), key1, loc1, false));
                ASSERT_OK(sorted->insert(opCtx.get(), key2, loc2, false));

                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> t1(harnessHelper->newOperationContext());
            scoped_ptr<OperationContext> t2(harnessHelper->newOperationContext());

            scoped_ptr<WriteUnitOfWork> w1(new WriteUnitOfWork(t1.get()));
            scoped_ptr<WriteUnitOfWork> w2(new WriteUnitOfWork(t2.get()));

            ASSERT_OK(sorted->insert(t1.get(), key3, loc3, false));
            ASSERT_OK(sorted->insert(t2.get(), key4, loc4, false));

            // this should throw
            ASSERT_THROWS(sorted->insert(t2.get(), key3, loc5, false), WriteConflictException);

            w1->commit();  // this should succeed
        }

        {
            scoped_ptr<OperationContext> t1(harnessHelper->newOperationContext());
            scoped_ptr<OperationContext> t2(harnessHelper->newOperationContext());

            scoped_ptr<WriteUnitOfWork> w2(new WriteUnitOfWork(t2.get()));
            // ensure we start w2 transaction
            ASSERT_OK(sorted->insert(t2.get(), key4, loc4, false));

            {
                scoped_ptr<WriteUnitOfWork> w1(new WriteUnitOfWork(t1.get()));

                {
                    WriteUnitOfWork w(t1.get());
                    ASSERT_OK(sorted->insert(t1.get(), key5, loc3, false));
                    w.commit();
                }
                w1->commit();
            }

            // this should throw
            ASSERT_THROWS(sorted->insert(t2.get(), key5, loc3, false), WriteConflictException);
        }
    }
}
