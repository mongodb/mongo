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

#include <boost/shared_ptr.hpp>
#include <boost/filesystem/operations.hpp>

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>

#include "mongo/db/storage/rocks/rocks_engine.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/db/storage/rocks/rocks_sorted_data_impl.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"

namespace mongo {

    class RocksSortedDataImplHarness : public HarnessHelper {
    public:
        RocksSortedDataImplHarness() : _order(Ordering::make(BSONObj())), _tempDir(_testNamespace) {
            boost::filesystem::remove_all(_tempDir.path());
            rocksdb::DB* db;
            std::vector<rocksdb::ColumnFamilyDescriptor> cfs;
            cfs.emplace_back();
            cfs.emplace_back("sroted_data_impl", rocksdb::ColumnFamilyOptions());
            cfs[1].options.comparator = RocksSortedDataImpl::newRocksComparator(_order);
            rocksdb::DBOptions db_options;
            db_options.create_if_missing = true;
            db_options.create_missing_column_families = true;
            std::vector<rocksdb::ColumnFamilyHandle*> handles;
            auto s = rocksdb::DB::Open(db_options, _tempDir.path(), cfs, &handles, &db);
            ASSERT(s.ok());
            _db.reset(db);
            _cf.reset(handles[1]);
        }

        virtual SortedDataInterface* newSortedDataInterface(bool unique) {
            return new RocksSortedDataImpl(_db.get(), _cf, rocksdb::kDefaultColumnFamilyName,
                                           _order);
        }

        virtual RecoveryUnit* newRecoveryUnit() { return new RocksRecoveryUnit(_db.get()); }

    private:
        Ordering _order;
        string _testNamespace = "mongo-rocks-sorted-data-test";
        unittest::TempDir _tempDir;
        scoped_ptr<rocksdb::DB> _db;
        shared_ptr<rocksdb::ColumnFamilyHandle> _cf;
    };

    HarnessHelper* newHarnessHelper() { return new RocksSortedDataImplHarness(); }
}
