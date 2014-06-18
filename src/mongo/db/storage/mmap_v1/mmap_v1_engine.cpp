// mmap_v1_engine.cpp

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

#include "mongo/db/storage/mmap_v1/mmap_v1_engine.h"

#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>

#include "mongo/db/storage_options.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_database_catalog_entry.h"
#include "mongo/db/storage/mmap_v1/dur_recovery_unit.h"
#include "mongo/util/mmap.h"

namespace mongo {

    MMAPV1Engine::~MMAPV1Engine() {
    }

    RecoveryUnit* MMAPV1Engine::newRecoveryUnit( OperationContext* opCtx ) {
        return new DurRecoveryUnit( opCtx );
    }

    void MMAPV1Engine::listDatabases( std::vector<std::string>* out ) const {
        _listDatabases( storageGlobalParams.dbpath, out );
    }

    DatabaseCatalogEntry* MMAPV1Engine::getDatabaseCatalogEntry( OperationContext* opCtx,
                                                                 const StringData& db ) {
        return new MMAPV1DatabaseCatalogEntry( opCtx,
                                               db,
                                               storageGlobalParams.dbpath,
                                               storageGlobalParams.directoryperdb,
                                               false );
    }

    void MMAPV1Engine::_listDatabases( const std::string& directory,
                                       std::vector<std::string>* out ) {
        boost::filesystem::path path( directory );
        for ( boost::filesystem::directory_iterator i( path );
              i != boost::filesystem::directory_iterator();
              ++i ) {
            if (storageGlobalParams.directoryperdb) {
                boost::filesystem::path p = *i;
                string dbName = p.leaf().string();
                p /= ( dbName + ".ns" );
                if ( exists( p ) )
                    out->push_back( dbName );
            }
            else {
                string fileName = boost::filesystem::path(*i).leaf().string();
                if ( fileName.length() > 3 && fileName.substr( fileName.length() - 3, 3 ) == ".ns" )
                    out->push_back( fileName.substr( 0, fileName.length() - 3 ) );
            }
        }
    }

    int MMAPV1Engine::flushAllFiles( bool sync ) {
        return MongoFile::flushAll( sync );
    }

}
