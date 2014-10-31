// mmap_v1_engine.h

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

#include <boost/thread/mutex.hpp>

#include "mongo/db/storage/storage_engine.h"

namespace mongo {

    class MMAPV1DatabaseCatalogEntry;

    class MMAPV1Engine : public StorageEngine {
    public:
        MMAPV1Engine();
        virtual ~MMAPV1Engine();

        void finishInit();

        RecoveryUnit* newRecoveryUnit( OperationContext* opCtx );
        void listDatabases( std::vector<std::string>* out ) const;
        int flushAllFiles( bool sync );

        Status repairDatabase( OperationContext* tnx,
                               const std::string& dbName,
                               bool preserveClonedFilesOnFailure,
                               bool backupOriginalFiles );

        DatabaseCatalogEntry* getDatabaseCatalogEntry( OperationContext* opCtx,
                                                       const StringData& db );

        virtual bool supportsDocLocking() const { return false; }
        virtual bool isMmapV1() const { return true; }

        virtual bool isDurable() const;

        Status closeDatabase(OperationContext* txn, const StringData& db );

        Status dropDatabase(OperationContext* txn, const StringData& db );

        void cleanShutdown(OperationContext* txn);

    private:
        static void _listDatabases( const std::string& directory,
                                    std::vector<std::string>* out );

        boost::mutex _entryMapMutex;
        typedef std::map<std::string,MMAPV1DatabaseCatalogEntry*> EntryMap;
        EntryMap _entryMap;
    };

    void _deleteDataFiles(const std::string& database);
}
