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

#include "mongo/platform/basic.h"

#include "mongo/s/cluster_write.h"

#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/s/chunk_manager_targeter.h"
#include "mongo/s/config.h"
#include "mongo/s/dbclient_multi_command.h"
#include "mongo/s/dbclient_shard_resolver.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/config_coordinator.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

    using std::vector;
    using std::string;

    const int ConfigOpTimeoutMillis = 30 * 1000;

    namespace {
        // TODO: consider writing a type for index instead
        /**
         * Constructs the BSON specification document for the given namespace, index key
         * and options.
         */
        BSONObj createIndexDoc( const string& ns, const BSONObj& keys, bool unique ) {
            BSONObjBuilder indexDoc;
            indexDoc.append( "ns" , ns );
            indexDoc.append( "key" , keys );

            stringstream indexName;

            bool isFirstKey = true;
            for ( BSONObjIterator keyIter(keys); keyIter.more(); ) {
                BSONElement currentKey = keyIter.next();

                if ( isFirstKey ) {
                    isFirstKey = false;
                }
                else {
                    indexName << "_";
                }

                indexName << currentKey.fieldName() << "_";
                if ( currentKey.isNumber() ) {
                    indexName << currentKey.numberInt();
                }
                else {
                    indexName << currentKey.str(); //this should match up with shell command
                }
            }

            indexDoc.append( "name", indexName.str() );

            if ( unique ) {
                indexDoc.appendBool( "unique", unique );
            }

            return indexDoc.obj();
        }
    }

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

    /**
     * Returns the currently-set config hosts for a cluster
     */
    static vector<ConnectionString> getConfigHosts() {

        vector<ConnectionString> configHosts;
        ConnectionString configHostOrHosts = configServer.getConnectionString();
        if ( configHostOrHosts.type() == ConnectionString::MASTER ) {
            configHosts.push_back( configHostOrHosts );
        }
        else if ( configHostOrHosts.type() == ConnectionString::SYNC ) {
            vector<HostAndPort> configHPs = configHostOrHosts.getServers();
            for ( vector<HostAndPort>::iterator it = configHPs.begin(); it != configHPs.end();
                ++it ) {
                configHosts.push_back( ConnectionString( *it ) );
            }
        }
        else {
            // This is only for tests.
            dassert( configHostOrHosts.type() == ConnectionString::CUSTOM );
            configHosts.push_back( configHostOrHosts );
        }

        return configHosts;
    }

    static Status getStatus( const BatchedCommandResponse& response ) {
        if ( response.getOk() != 1 ) {
            return Status( static_cast<ErrorCodes::Error>(response.getErrCode()),
                           response.getErrMessage() );
        }

        if ( response.isErrDetailsSet() ) {
            const WriteErrorDetail* errDetail = response.getErrDetails().front();
            return Status( static_cast<ErrorCodes::Error>(errDetail->getErrCode()),
                           errDetail->getErrMessage() );
        }

        if ( response.isWriteConcernErrorSet() ) {
            const WCErrorDetail* errDetail = response.getWriteConcernError();
            return Status( static_cast<ErrorCodes::Error>(errDetail->getErrCode()),
                           errDetail->getErrMessage() );
        }

        return Status::OK();
    }

    Status clusterInsert( const string& ns,
                          const BSONObj& doc,
                          const BSONObj& writeConcern,
                          BatchedCommandResponse* response ) {
        auto_ptr<BatchedInsertRequest> insert( new BatchedInsertRequest() );
        insert->addToDocuments( doc );

        BatchedCommandRequest request( insert.release() );
        request.setNS( ns );
        if ( !writeConcern.isEmpty() ) {
            request.setWriteConcern( writeConcern );
        }

        BatchedCommandResponse dummyResponse;

        if ( response == NULL ) {
            response = &dummyResponse;
        }

        clusterWrite( request, response, false );
        return getStatus( *response );
    }

    Status clusterUpdate( const string& ns,
                          const BSONObj& query,
                          const BSONObj& update,
                          bool upsert,
                          bool multi,
                          const BSONObj& writeConcern,
                          BatchedCommandResponse* response ) {
        auto_ptr<BatchedUpdateDocument> updateDoc( new BatchedUpdateDocument() );
        updateDoc->setQuery( query );
        updateDoc->setUpdateExpr( update );
        updateDoc->setUpsert( upsert );
        updateDoc->setMulti( multi );

        auto_ptr<BatchedUpdateRequest> updateRequest( new BatchedUpdateRequest() );
        updateRequest->addToUpdates( updateDoc.release() );

        if ( !writeConcern.isEmpty() ) {
            updateRequest->setWriteConcern( writeConcern );
        }

        BatchedCommandRequest request( updateRequest.release() );
        request.setNS( ns );

        BatchedCommandResponse dummyResponse;

        if ( response == NULL ) {
            response = &dummyResponse;
        }

        clusterWrite( request, response, false );
        return getStatus( *response );
    }

    Status clusterDelete( const string& ns,
                          const BSONObj& query,
                          int limit,
                          const BSONObj& writeConcern,
                          BatchedCommandResponse* response ) {
        auto_ptr<BatchedDeleteDocument> deleteDoc( new BatchedDeleteDocument );
        deleteDoc->setQuery( query );
        deleteDoc->setLimit( limit );

        auto_ptr<BatchedDeleteRequest> deleteRequest( new BatchedDeleteRequest() );
        deleteRequest->addToDeletes( deleteDoc.release() );

        if ( !writeConcern.isEmpty() ) {
            deleteRequest->setWriteConcern( writeConcern );
        }

        BatchedCommandRequest request( deleteRequest.release() );
        request.setNS( ns );

        BatchedCommandResponse dummyResponse;

        if ( response == NULL ) {
            response = &dummyResponse;
        }

        clusterWrite( request, response, false );
        return getStatus( *response );
    }

    Status clusterCreateIndex( const string& ns,
                               BSONObj keys,
                               bool unique,
                               const BSONObj& writeConcern,
                               BatchedCommandResponse* response ) {
        return clusterInsert( NamespaceString( ns ).getSystemIndexesCollection(),
                              createIndexDoc( ns, keys, unique ),
                              writeConcern,
                              response );
    }

    void clusterWrite( const BatchedCommandRequest& request,
                       BatchedCommandResponse* response,
                       bool autoSplit ) {
        ClusterWriter writer( autoSplit, 0 );
        writer.write( request, response );
    }

    bool validConfigWC( const BSONObj& writeConcern ) {
        BSONElement elem(writeConcern["w"]);

        if ( elem.eoo() ) {
            return true;
        }

        if ( elem.isNumber() && elem.numberInt() <= 1 ) {
            return true;
        }

        if ( elem.type() == String && elem.str() == "majority" ) {
            return true;
        }

        return false;
    }

    static void toBatchError( const Status& status, BatchedCommandResponse* response ) {
        response->clear();
        response->setErrCode( status.code() );
        response->setErrMessage( status.reason() );
        response->setOk( false );
        dassert( response->isValid(NULL) );
    }

    void ClusterWriter::write( const BatchedCommandRequest& origRequest,
                               BatchedCommandResponse* response ) {

        // Add _ids to insert request if req'd
        auto_ptr<BatchedCommandRequest> idRequest(BatchedCommandRequest::cloneWithIds(origRequest));
        const BatchedCommandRequest& request = NULL != idRequest.get() ? *idRequest : origRequest;

        const NamespaceString nss = NamespaceString( request.getNS() );
        if ( !nss.isValid() ) {
            toBatchError( Status( ErrorCodes::InvalidNamespace,
                                  nss.ns() + " is not a valid namespace" ),
                          response );
            return;
        }

        if ( !NamespaceString::validCollectionName( nss.coll() ) ) {
            toBatchError( Status( ErrorCodes::BadValue,
                                  str::stream() << "invalid collection name " << nss.coll() ),
                          response );
            return;
        }

        if ( request.sizeWriteOps() == 0u ) {
            toBatchError( Status( ErrorCodes::InvalidLength,
                                  "no write ops were included in the batch" ),
                          response );
            return;
        }

        if ( request.sizeWriteOps() > BatchedCommandRequest::kMaxWriteBatchSize ) {
            toBatchError( Status( ErrorCodes::InvalidLength,
                                  str::stream() << "exceeded maximum write batch size of "
                                                << BatchedCommandRequest::kMaxWriteBatchSize ),
                          response );
            return;
        }

        string errMsg;
        if ( request.isInsertIndexRequest() && !request.isValidIndexRequest( &errMsg ) ) {
            toBatchError( Status( ErrorCodes::InvalidOptions, errMsg ), response );
            return;
        }

        // Config writes and shard writes are done differently
        string dbName = nss.db().toString();
        if ( dbName == "config" || dbName == "admin" ) {

            bool verboseWC = request.isVerboseWC();

            // We only support batch sizes of one for config writes
            if ( request.sizeWriteOps() != 1 ) {
                toBatchError( Status( ErrorCodes::InvalidOptions,
                                      mongoutils::str::stream() << "Writes to config servers must "
                                              "have batch size of 1, found "
                                              << request.sizeWriteOps() ),
                              response );
                return;
            }

            // We only support {w: 0}, {w: 1}, and {w: 'majority'} write concern for config writes
            if ( request.isWriteConcernSet() && !validConfigWC( request.getWriteConcern() )) {
                toBatchError( Status( ErrorCodes::InvalidOptions,
                                      mongoutils::str::stream() << "Invalid write concern for write"
				              " to config servers: " << request.getWriteConcern() ),
                              response );
                return;
            }

            // We need to support "best-effort" writes for pings to the config server.
            // {w:0} (!verbose) writes are interpreted as best-effort in this case - they may still
            // error, but do not do the initial fsync check.
            configWrite( request, response, verboseWC );
        }
        else {
            shardWrite( request, response );
        }
    }

    ClusterWriter::ClusterWriter( bool autoSplit, int timeoutMillis ) :
        _autoSplit( autoSplit ), _timeoutMillis( timeoutMillis ), _stats( new ClusterWriterStats ) {
    }

    const ClusterWriterStats& ClusterWriter::getStats() {
        return *_stats;
    }

    void ClusterWriter::shardWrite( const BatchedCommandRequest& request,
                                    BatchedCommandResponse* response ) {

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

        if ( _autoSplit )
            splitIfNeeded( request.getNS(), *targeter.getStats() );

        _stats->setShardStats( exec.releaseStats() );
    }

    void ClusterWriter::configWrite( const BatchedCommandRequest& request,
                                     BatchedCommandResponse* response,
                                     bool fsyncCheck ) {

        DBClientMultiCommand dispatcher;
        ConfigCoordinator exec( &dispatcher, getConfigHosts() );
        exec.executeBatch( request, response, fsyncCheck );
    }

    void ClusterWriterStats::setShardStats( BatchWriteExecStats* shardStats ) {
        _shardStats.reset( shardStats );
    }

    bool ClusterWriterStats::hasShardStats() const {
        return NULL != _shardStats.get();
    }

    const BatchWriteExecStats& ClusterWriterStats::getShardStats() const {
        return *_shardStats;
    }

} // namespace mongo
