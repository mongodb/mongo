// wiredtiger_engine.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *    Copyright (C) 2014 WiredTiger Inc.
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

#include <set>
#include <string>
#include <map>

#include <boost/thread/mutex.hpp>

#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_database.h"

namespace mongo {

    class WiredTigerDatabaseCatalogEntry;

    class WiredTigerEngine : public StorageEngine {
    public:
        WiredTigerEngine(const std::string &);

        virtual ~WiredTigerEngine() {}

        virtual void cleanShutdown( OperationContext* txn );

        virtual RecoveryUnit* newRecoveryUnit( OperationContext* txn );

        virtual void listDatabases( std::vector<std::string>* out ) const;

        virtual Status closeDatabase(OperationContext*, const StringData&);

        virtual Status dropDatabase(OperationContext*, const StringData&);

        virtual DatabaseCatalogEntry* getDatabaseCatalogEntry( OperationContext* txn,
                                                               const StringData& db );

        /**
         * @return number of files flushed
         */
        virtual int flushAllFiles( bool sync ) { return 0; }

        virtual Status repairDatabase( OperationContext* tnx,
                                       const std::string& dbName,
                                       bool preserveClonedFilesOnFailure = false,
                                       bool backupOriginalFiles = false ) { return Status::OK(); }

	virtual bool supportsDocLocking() const { return true; }

    private:
        void loadExistingDatabases();

        std::string _path;
        WiredTigerDatabase *_db;

        mutable boost::mutex _dbLock;
        typedef std::map<std::string, WiredTigerDatabaseCatalogEntry *> DBMap;
        DBMap _dbs;
    };

}
