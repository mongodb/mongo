/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/distlock.h"
#include "mongo/s/chunk.h"  // needed for genID
#include "mongo/s/config.h" // needed for changelog write
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using std::string;
    using mongoutils::str::stream;

    static BSONObj buildApplyOpsCmd( const OwnedPointerVector<ChunkType>&,
                                     const ChunkVersion&,
                                     const ChunkVersion& );

    static BSONObj buildMergeLogEntry( const OwnedPointerVector<ChunkType>&,
                                       const ChunkVersion&,
                                       const ChunkVersion& );

    static bool isEmptyChunk( const ChunkType& );

    bool mergeChunks( const NamespaceString& nss,
                      const BSONObj& minKey,
                      const BSONObj& maxKey,
                      const OID& epoch,
                      string* errMsg ) {

        //
        // Get sharding state up-to-date
        //

        ConnectionString configLoc = ConnectionString::parse( shardingState.getConfigServer(),
                                                              *errMsg );
        if ( !configLoc.isValid() ){
            warning() << *errMsg << endl;
            return false;
        }

        //
        // Get the distributed lock
        //

        ScopedDistributedLock collLock( configLoc, nss.ns() );
        collLock.setLockMessage( stream() << "merging chunks in " << nss.ns() << " from "
                                          << minKey << " to " << maxKey );

        if ( !collLock.tryAcquire( errMsg ) ) {

            *errMsg = stream() << "could not acquire collection lock for " << nss.ns()
                               << " to merge chunks in [" << minKey << "," << maxKey << ")"
                               << causedBy( *errMsg );

            warning() << *errMsg << endl;
            return false;
        }

        //
        // We now have the collection lock, refresh metadata to latest version and sanity check
        //

        ChunkVersion shardVersion;
        Status status = shardingState.refreshMetadataNow( nss.ns(), &shardVersion );

        if ( !status.isOK() ) {

            *errMsg = str::stream() << "could not merge chunks, failed to refresh metadata for "
                                    << nss.ns() << causedBy( status.reason() );

            warning() << *errMsg << endl;
            return false;
        }

        if ( epoch.isSet() && shardVersion.epoch() != epoch ) {

            *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                               << " has changed" << " since merge was sent" << "(sent epoch : "
                               << epoch.toString()
                               << ", current epoch : " << shardVersion.epoch().toString() << ")";

            warning() << *errMsg << endl;
            return false;
        }

        CollectionMetadataPtr metadata = shardingState.getCollectionMetadata( nss.ns() );

        if ( !metadata || metadata->getKeyPattern().isEmpty() ) {

            *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                               << " is not sharded";

            warning() << *errMsg << endl;
            return false;
        }

        dassert( metadata->getShardVersion().isEquivalentTo( shardVersion ) );

        if ( !metadata->isValidKey( minKey ) || !metadata->isValidKey( maxKey ) ) {

            *errMsg = stream() << "could not merge chunks, the range "
                               << rangeToString( minKey, maxKey ) << " is not valid"
                               << " for collection " << nss.ns() << " with key pattern "
                               << metadata->getKeyPattern();

            warning() << *errMsg << endl;
            return false;
        }

        //
        // Get merged chunk information
        //

        ChunkVersion mergeVersion = metadata->getCollVersion();
        mergeVersion.incMinor();

        OwnedPointerVector<ChunkType> chunksToMerge;

        ChunkType itChunk;
        itChunk.setMin( minKey );
        itChunk.setMax( minKey );
        itChunk.setNS( nss.ns() );
        itChunk.setShard( shardingState.getShardName() );

        while ( itChunk.getMax().woCompare( maxKey ) < 0 &&
                metadata->getNextChunk( itChunk.getMax(), &itChunk ) ) {
            auto_ptr<ChunkType> saved( new ChunkType );
            itChunk.cloneTo( saved.get() );
            chunksToMerge.mutableVector().push_back( saved.release() );
        }

        if ( chunksToMerge.empty() ) {

            *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                               << " range starting at " << minKey
                               << " and ending at " << maxKey
                               << " does not belong to shard " << shardingState.getShardName();

            warning() << *errMsg << endl;
            return false;
        }

        //
        // Validate the range starts and ends at chunks and has no holes, error if not valid
        //

        BSONObj firstDocMin = ( *chunksToMerge.begin() )->getMin();
        BSONObj firstDocMax = ( *chunksToMerge.begin() )->getMax();
        // minKey is inclusive
        bool minKeyInRange = rangeContains( firstDocMin, firstDocMax, minKey );

        if ( !minKeyInRange ) {

            *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                               << " range starting at " << minKey
                               << " does not belong to shard " << shardingState.getShardName();

            warning() << *errMsg << endl;
            return false;
        }

        BSONObj lastDocMin = ( *chunksToMerge.rbegin() )->getMin();
        BSONObj lastDocMax = ( *chunksToMerge.rbegin() )->getMax();
        // maxKey is exclusive
        bool maxKeyInRange = lastDocMin.woCompare( maxKey ) < 0 &&
                lastDocMax.woCompare( maxKey ) >= 0;

        if ( !maxKeyInRange ) {
            *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                               << " range ending at " << maxKey
                               << " does not belong to shard " << shardingState.getShardName();

            warning() << *errMsg << endl;
            return false;
        }

        bool validRangeStartKey = firstDocMin.woCompare( minKey ) == 0;
        bool validRangeEndKey = lastDocMax.woCompare( maxKey ) == 0;

        if ( !validRangeStartKey || !validRangeEndKey ) {

            *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                               << " does not contain a chunk "
                               << ( !validRangeStartKey ? "starting at " + minKey.toString() : "" )
                               << ( !validRangeStartKey && !validRangeEndKey ? " or " : "" )
                               << ( !validRangeEndKey ? "ending at " + maxKey.toString() : "" );

            warning() << *errMsg << endl;
            return false;
        }

        if ( chunksToMerge.size() == 1 ) {

            *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                               << " already contains chunk for " << rangeToString( minKey, maxKey );

            warning() << *errMsg << endl;
            return false;
        }

        bool holeInRange = false;

        // Look for hole in range
        ChunkType* prevChunk = *chunksToMerge.begin();
        ChunkType* nextChunk = NULL;
        for ( OwnedPointerVector<ChunkType>::const_iterator it = chunksToMerge.begin();
                it != chunksToMerge.end(); ++it ) {
            if ( it == chunksToMerge.begin() ) continue;

            nextChunk = *it;
            if ( prevChunk->getMax().woCompare( nextChunk->getMin() ) != 0 ) {
                holeInRange = true;
                break;
            }
            prevChunk = nextChunk;
        }

        if ( holeInRange ) {

            dassert( NULL != nextChunk );
            *errMsg = stream() << "could not merge chunks, collection " << nss.ns()
                               << " has a hole in the range " << rangeToString( minKey, maxKey )
                               << " at " << rangeToString( prevChunk->getMax(),
                                                           nextChunk->getMin() );

            warning() << *errMsg << endl;
            return false;
        }

        //
        // Run apply ops command
        //

        BSONObj applyOpsCmd = buildApplyOpsCmd( chunksToMerge,
                                                shardVersion,
                                                mergeVersion );

        bool ok;
        BSONObj result;
        try {
            ScopedDbConnection conn( configLoc, 30.0 );
            ok = conn->runCommand( "config", applyOpsCmd, result );
            if ( !ok ) *errMsg = result.toString();
            conn.done();
        }
        catch( const DBException& ex ) {
            ok = false;
            *errMsg = ex.toString();
        }

        if ( !ok ) {
            *errMsg = stream() << "could not merge chunks for " << nss.ns()
                               << ", writing to config failed" << causedBy( errMsg );

            warning() << *errMsg << endl;
            return false;
        }

        //
        // Install merged chunk metadata
        //

        {
            Lock::DBWrite writeLk( nss.ns() );
            shardingState.mergeChunks( nss.ns(), minKey, maxKey, mergeVersion );
        }

        //
        // Log change
        //

        BSONObj mergeLogEntry = buildMergeLogEntry( chunksToMerge,
                                                    shardVersion,
                                                    mergeVersion );

        configServer.logChange( "merge", nss.ns(), mergeLogEntry );

        return true;
    }

    //
    // Utilities for building BSONObjs for applyOps and change logging
    //

    BSONObj buildMergeLogEntry( const OwnedPointerVector<ChunkType>& chunksToMerge,
                                const ChunkVersion& currShardVersion,
                                const ChunkVersion& newMergedVersion ) {

        BSONObjBuilder logDetailB;

        BSONArrayBuilder mergedB( logDetailB.subarrayStart( "merged" ) );

        for ( OwnedPointerVector<ChunkType>::const_iterator it = chunksToMerge.begin();
                it != chunksToMerge.end(); ++it ) {
            ChunkType* chunkToMerge = *it;
            mergedB.append( chunkToMerge->toBSON() );
        }

        mergedB.done();

        currShardVersion.addToBSON( logDetailB, "prevShardVersion" );
        newMergedVersion.addToBSON( logDetailB, "mergedVersion" );

        return logDetailB.obj();
    }


    BSONObj buildOpMergeChunk( const ChunkType& mergedChunk ) {

        BSONObjBuilder opB;

        // Op basics
        opB.append( "op" , "u" );
        opB.appendBool( "b" , false ); // no upserting
        opB.append( "ns" , ChunkType::ConfigNS );

        // New object
        opB.append( "o", mergedChunk.toBSON() );

        // Query object
        opB.append( "o2",
                    BSON( ChunkType::name( mergedChunk.getName() ) ) );

        return opB.obj();
    }

    BSONObj buildOpRemoveChunk( const ChunkType& chunkToRemove ) {

        BSONObjBuilder opB;

        // Op basics
        opB.append( "op", "d" ); // delete
        opB.append( "ns", ChunkType::ConfigNS );

        opB.append( "o", BSON( ChunkType::name( chunkToRemove.getName() ) ) );

        return opB.obj();
    }

    BSONObj buildOpPrecond( const string& ns,
                            const string& shardName,
                            const ChunkVersion& shardVersion ) {
        BSONObjBuilder condB;
        condB.append( "ns", ChunkType::ConfigNS );
        condB.append( "q", BSON( "query" << BSON( ChunkType::ns( ns ) )
                              << "orderby" << BSON( ChunkType::DEPRECATED_lastmod() << -1 ) ) );
        {
            BSONObjBuilder resB( condB.subobjStart( "res" ) );
            shardVersion.addToBSON( resB, ChunkType::DEPRECATED_lastmod() );
            resB.done();
        }

        return condB.obj();
    }

    BSONObj buildApplyOpsCmd( const OwnedPointerVector<ChunkType>& chunksToMerge,
                              const ChunkVersion& currShardVersion,
                              const ChunkVersion& newMergedVersion ) {

        BSONObjBuilder applyOpsCmdB;
        BSONArrayBuilder updatesB( applyOpsCmdB.subarrayStart( "applyOps" ) );

        // The chunk we'll be "expanding" is the first chunk
        const ChunkType* chunkToMerge = *chunksToMerge.begin();

        // Fill in details not tracked by metadata
        ChunkType mergedChunk;
        chunkToMerge->cloneTo( &mergedChunk );
        mergedChunk.setName( Chunk::genID( chunkToMerge->getNS(), chunkToMerge->getMin() ) );
        mergedChunk.setMax( ( *chunksToMerge.vector().rbegin() )->getMax() );
        mergedChunk.setVersion( newMergedVersion );

        updatesB.append( buildOpMergeChunk( mergedChunk ) );

        // Don't remove chunk we're expanding
        OwnedPointerVector<ChunkType>::const_iterator it = chunksToMerge.begin();
        for ( ++it; it != chunksToMerge.end(); ++it ) {
            ChunkType* chunkToMerge = *it;
            chunkToMerge->setName( Chunk::genID( chunkToMerge->getNS(), chunkToMerge->getMin() ) );
            updatesB.append( buildOpRemoveChunk( *chunkToMerge ) );
        }

        updatesB.done();

        applyOpsCmdB.append( "preCondition",
                             buildOpPrecond( chunkToMerge->getNS(),
                                             chunkToMerge->getShard(),
                                             currShardVersion ) );

        return applyOpsCmdB.obj();
    }

}
