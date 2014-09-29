// wiredtiger_index_test.cpp

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

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_database.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_database_catalog_entry.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    class MyHarnessHelper : public HarnessHelper {
    public:
        MyHarnessHelper() :
             _td("wiredtiger-test"), _db(NULL), _info(NULL) {

            const char * def_config = ",create,extensions=[local=(entry=index_collator_extension)]";
            std::string config = std::string( wiredTigerGlobalOptions.databaseConfig + def_config );

            WT_CONNECTION* conn;
            int ret = wiredtiger_open( _td.path().c_str(), NULL, config.c_str(), &conn );
            invariantWTOK(ret);

            _db.reset( new WiredTigerDatabase( conn ) );
        }

        virtual SortedDataInterface* newSortedDataInterface() {
            std::string ns = "test.wt";
            WiredTigerDatabaseCatalogEntry d( *_db, ns );

            OperationContext* txn = NULL;
            CollectionCatalogEntry* coll = d.getCollectionCatalogEntry( txn, ns );

            BSONObj spec = BSON( "key" << BSON( "a" << 1 ) <<
                                 "name" << "testIndex" <<
                                 "ns" << ns );

            IndexDescriptor desc( NULL, "", spec );
            _info = new IndexCatalogEntry( ns, coll, &desc, NULL );

            return getWiredTigerIndex( *_db, "test", "testIndex", *_info );
        }

        virtual RecoveryUnit* newRecoveryUnit() {
            return new WiredTigerRecoveryUnit( *_db, true );
        }

    private:
        unittest::TempDir _td;

        scoped_ptr<WiredTigerDatabase> _db;
        IndexCatalogEntry* _info;
    };

    HarnessHelper* newHarnessHelper() {
        return new MyHarnessHelper();
    }

}
