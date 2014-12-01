// kv_storage_engine.h

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

#pragma once

#include <map>
#include <string>

#include <boost/scoped_ptr.hpp>
#include <boost/thread/mutex.hpp>

#include "mongo/db/storage/kv/kv_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"

namespace mongo {

    class KVCatalog;
    class KVEngine;
    class KVDatabaseCatalogEntry;

    struct KVStorageEngineOptions {
        KVStorageEngineOptions() :
            directoryPerDB(false),
            splitCollectionAndIndexes(false) {}

        bool directoryPerDB;
        bool splitCollectionAndIndexes;
    };

    class KVStorageEngine : public StorageEngine {
    public:
        /**
         * @param engine - owneership passes to me
         */
        KVStorageEngine( KVEngine* engine,
                         const KVStorageEngineOptions& options = KVStorageEngineOptions() );
        virtual ~KVStorageEngine();

        virtual void finishInit();

        virtual RecoveryUnit* newRecoveryUnit( OperationContext* opCtx );

        virtual void listDatabases( std::vector<std::string>* out ) const;

        virtual DatabaseCatalogEntry* getDatabaseCatalogEntry( OperationContext* opCtx,
                                                               const StringData& db );

        virtual bool supportsDocLocking() const { return _supportsDocLocking; }

        virtual Status closeDatabase( OperationContext* txn, const StringData& db );

        virtual Status dropDatabase( OperationContext* txn, const StringData& db );

        virtual int flushAllFiles( bool sync );

        virtual bool isDurable() const;

        virtual Status repairDatabase( OperationContext* txn,
                                       const std::string& dbName,
                                       bool preserveClonedFilesOnFailure = false,
                                       bool backupOriginalFiles = false );

        virtual void cleanShutdown(OperationContext* txn);

        // ------ kv ------

        KVEngine* getEngine() { return _engine.get(); }
        const KVEngine* getEngine() const { return _engine.get(); }

        KVCatalog* getCatalog() { return _catalog.get(); }
        const KVCatalog* getCatalog() const { return _catalog.get(); }

    private:
        class RemoveDBChange;

        KVStorageEngineOptions _options;

        // This must be the first member so it is destroyed last.
        boost::scoped_ptr<KVEngine> _engine;

        const bool _supportsDocLocking;

        boost::scoped_ptr<RecordStore> _catalogRecordStore;
        boost::scoped_ptr<KVCatalog> _catalog;

        typedef std::map<std::string,KVDatabaseCatalogEntry*> DBMap;
        DBMap _dbs;
        mutable boost::mutex _dbsLock;
    };

}
