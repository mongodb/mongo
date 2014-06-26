// storage_engine.h

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

#include <string>
#include <vector>

#include "mongo/base/status.h"

namespace mongo {

    class DatabaseCatalogEntry;
    class OperationContext;
    class RecoveryUnit;
    struct StorageGlobalParams;

    class StorageEngine {
    public:
        virtual ~StorageEngine() {}

        virtual RecoveryUnit* newRecoveryUnit( OperationContext* opCtx ) = 0;

        virtual void listDatabases( std::vector<std::string>* out ) const = 0;

        /**
         * TODO: document ownership semantics
         */
        virtual DatabaseCatalogEntry* getDatabaseCatalogEntry( OperationContext* opCtx,
                                                               const StringData& db ) = 0;

        /**
         * @return number of files flushed
         */
        virtual int flushAllFiles( bool sync ) = 0;

        virtual Status repairDatabase( OperationContext* tnx,
                                       const std::string& dbName,
                                       bool preserveClonedFilesOnFailure = false,
                                       bool backupOriginalFiles = false ) = 0;

        class Factory {
        public:
            virtual ~Factory(){}
            virtual StorageEngine* create( const StorageGlobalParams& params ) const = 0;
        };

        static void registerFactory( const std::string& name, const Factory* factory );
    };

    // TODO: this is temporary
    extern StorageEngine* globalStorageEngine;
}
