/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/s/cluster_write.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/s/batch_write_exec.h"
#include "mongo/s/chunk_manager_targeter.h"
#include "mongo/s/config.h"
#include "mongo/s/dbclient_multi_command.h"
#include "mongo/s/dbclient_shard_resolver.h"
#include "mongo/s/grid.h"

namespace mongo {

    /**
     * Splits the chunks touched based from the targeter stats if needed.
     */
    static void splitIfNeeded( const string& ns, const TargeterStats& stats ) {
        if ( !Chunk::ShouldAutoSplit ) {
            return;
        }

        DBConfigPtr config;

        try {
            config = grid.getDBConfig( ns );
        }
        catch ( const DBException& ex ) {
            warning() << "failed to get database config for " << ns
                      << " while checking for auto-split: " << causedBy( ex ) << endl;
            return;
        }

        ChunkManagerPtr chunkManager;
        ShardPtr dummyShard;
        config->getChunkManagerOrPrimary( ns, chunkManager, dummyShard );

        if ( !chunkManager ) {
            return;
        }

        for ( map<BSONObj, int>::const_iterator it = stats.chunkSizeDelta.begin();
            it != stats.chunkSizeDelta.end(); ++it ) {

            ChunkPtr chunk;
            try {
                chunk = chunkManager->findIntersectingChunk( it->first );
            }
            catch ( const AssertionException& ex ) {
                warning() << "could not find chunk while checking for auto-split: "
                          << causedBy( ex ) << endl;
                return;
            }

            chunk->splitIfShould( it->second );
        }
    }

    void clusterWrite( const BatchedCommandRequest& request,
                       BatchedCommandResponse* response,
                       bool autoSplit ) {

        ChunkManagerTargeter targeter;
        Status targetInitStatus = targeter.init( NamespaceString( request.getTargetingNS() ) );

        if ( !targetInitStatus.isOK() ) {

            warning() << "could not initialize targeter for"
                      << ( request.isInsertIndexRequest() ? " index" : "" )
                      << " write op in collection " << request.getTargetingNS() << endl;

            // Errors will be reported in response if we are unable to target
        }

        DBClientShardResolver resolver;
        DBClientMultiCommand dispatcher;

        BatchWriteExec exec( &targeter, &resolver, &dispatcher );

        exec.executeBatch( request, response );

        if ( autoSplit ) splitIfNeeded( request.getNS(), *targeter.getStats() );
    }

} // namespace mongo
