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

#include "mongo/unittest/unittest.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_engine.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"

namespace mongo {

    class WiredTigerHarnessHelper : public HarnessHelper {
    public:
        WiredTigerHarnessHelper() : _dbpath( "wt_test" ) {
            _engine.reset( new WiredTigerEngine( _dbpath.path() ) );
        }

        virtual RecordStore* newNonCappedRecordStore() {
            std::string ns = "a.b";
            OperationContext* txn = NULL;  // unused in this case
            DatabaseCatalogEntry* dce = _engine->getDatabaseCatalogEntry( txn, ns );

            Status status = dce->createCollection( txn, ns, CollectionOptions(), true );
            invariant( status == Status::OK() );

            RecordStore* rs = dce->getRecordStore( txn, ns );
            invariant( rs );
            return rs;
        }

        virtual RecoveryUnit* newRecoveryUnit() {
            OperationContext* txn = NULL;  // unused in this case
            return _engine->newRecoveryUnit( txn );
        }
    private:
        unittest::TempDir _dbpath;
        scoped_ptr<WiredTigerEngine> _engine;
    };

    HarnessHelper* newHarnessHelper() {
        return new WiredTigerHarnessHelper();
    }


}
