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

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    class WiredTigerHarnessHelper : public HarnessHelper {
    public:
        WiredTigerHarnessHelper() : _dbpath( "wt_test" ), _conn( NULL ) {

            const char* config = "create";
            int ret = wiredtiger_open( _dbpath.path().c_str(), NULL, config, &_conn);
            invariantWTOK( ret );

            _sessionCache = new WiredTigerSessionCache( _conn );
        }

        ~WiredTigerHarnessHelper() {
            delete _sessionCache;
            _conn->close(_conn, NULL);
        }

        virtual RecordStore* newNonCappedRecordStore() {
            std::string ns = "a.b";

            WiredTigerRecoveryUnit* ru = new WiredTigerRecoveryUnit( _sessionCache );
            OperationContextNoop txn( ru );
            string uri = "table:a.b";
            std::string config = WiredTigerRecordStore::generateCreateString(CollectionOptions(), "");

            {
                WriteUnitOfWork uow(&txn);
                WT_SESSION* s = ru->getSession()->getSession();
                invariantWTOK( s->create( s, uri.c_str(), config.c_str() ) );
                uow.commit();
            }

            return new WiredTigerRecordStore( &txn, ns, uri );
        }

        virtual RecordStore* newCappedRecordStore( int64_t cappedMaxSize,
                                                   int64_t cappedMaxDocs ) {
            std::string ns = "a.b";

            WiredTigerRecoveryUnit* ru = new WiredTigerRecoveryUnit( _sessionCache );
            OperationContextNoop txn( ru );
            string uri = "table:a.b";
            std::string config = WiredTigerRecordStore::generateCreateString(CollectionOptions(), "");

            {
                WriteUnitOfWork uow(&txn);
                WT_SESSION* s = ru->getSession()->getSession();
                invariantWTOK( s->create( s, uri.c_str(), config.c_str() ) );
                uow.commit();
            }

            return new WiredTigerRecordStore( &txn, ns, uri, true, cappedMaxSize, cappedMaxDocs );
        }

        virtual RecoveryUnit* newRecoveryUnit() {
            return new WiredTigerRecoveryUnit( _sessionCache );
        }
    private:
        unittest::TempDir _dbpath;
        WT_CONNECTION* _conn;
        WiredTigerSessionCache* _sessionCache;
    };

    HarnessHelper* newHarnessHelper() {
        return new WiredTigerHarnessHelper();
    }


}
