// @file writeback_listener.cpp

/**
*    Copyright (C) 2010 10gen Inc.
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
*/

#include "pch.h"

#include "../util/timer.h"

#include "mongo/db/auth/authorization_manager.h"
#include "config.h"
#include "grid.h"
#include "request.h"
#include "server.h"
#include "shard.h"
#include "util.h"
#include "client_info.h"

#include "writeback_listener.h"

namespace mongo {

    map<string,WriteBackListener*> WriteBackListener::_cache;
    set<string> WriteBackListener::_seenSets;
    mongo::mutex WriteBackListener::_cacheLock("WriteBackListener");

    map<WriteBackListener::ConnectionIdent,WriteBackListener::WBStatus> WriteBackListener::_seenWritebacks;
    mongo::mutex WriteBackListener::_seenWritebacksLock("WriteBackListener::seen");

    WriteBackListener::WriteBackListener( const string& addr ) : _addr( addr ) {
        _name = str::stream() << "WriteBackListener-" << addr;
        log() << "creating WriteBackListener for: " << addr << " serverID: " << serverID << endl;
    }

    /* static */
    void WriteBackListener::init( DBClientBase& conn ) {

        if ( conn.type() == ConnectionString::SYNC ) {
            // don't want write back listeners for config servers
            return;
        }

        if ( conn.type() != ConnectionString::SET ) {
            init( conn.getServerAddress() );
            return;
        }

        {
            scoped_lock lk( _cacheLock );
            if ( _seenSets.count( conn.getServerAddress() ) )
                return;
        }

        // we want to do writebacks on all rs nodes
        string errmsg;
        ConnectionString cs = ConnectionString::parse( conn.getServerAddress() , errmsg );
        uassert( 13641 , str::stream() << "can't parse host [" << conn.getServerAddress() << "]" , cs.isValid() );

        vector<HostAndPort> hosts = cs.getServers();

        for ( unsigned i=0; i<hosts.size(); i++ )
            init( hosts[i].toString() );

    }

    /* static */
    void WriteBackListener::init( const string& host ) {
        scoped_lock lk( _cacheLock );
        WriteBackListener*& l = _cache[host];
        if ( l )
            return;
        l = new WriteBackListener( host );
        l->go();
    }

    /* static */
    BSONObj WriteBackListener::waitFor( const ConnectionIdent& ident, const OID& oid ) {

        Timer t;
        Timer lastMessageTimer;

        while ( t.minutes() < 60 ) {
            {
                scoped_lock lk( _seenWritebacksLock );
                WBStatus s = _seenWritebacks[ident];

                if ( oid < s.id ) {
                    // this means we're waiting for a GLE that already passed.
                    // it should be impossible because once we call GLE, no other
                    // writebacks should happen with that connection id

                    msgasserted( 14041 , str::stream() << "got writeback waitfor for older id " <<
                                 " oid: " << oid << " s.id: " << s.id << " ident: " << ident.toString() );
                }
                else if ( oid == s.id ) {
                    return s.gle;
                }

                // Stay in lock so we can use the status
                if( lastMessageTimer.seconds() > 10 ){

                    warning() << "waiting for writeback " << oid
                              << " from connection " << ident.toString()
                              << " for " << t.seconds() << " secs"
                              << ", currently at id " << s.id << endl;

                    lastMessageTimer.reset();
                }
            }

            sleepmillis( 10 );
        }

        uasserted( 13403 , str::stream() << "didn't get writeback for: " << oid
                                         << " after: " << t.millis() << " ms"
                                         << " from connection " << ident.toString() );

        throw 1; // never gets here
    }

    void WriteBackListener::run() {

        int secsToSleep = 0;
        scoped_ptr<ShardChunkVersion> lastNeededVersion;
        int lastNeededCount = 0;
        bool needsToReloadShardInfo = false;

        while ( ! inShutdown() ) {

            if ( ! Shard::isAShardNode( _addr ) ) {
                LOG(1) << _addr << " is not a shard node" << endl;
                sleepsecs( 60 );
                continue;
            }

            try {
                if (needsToReloadShardInfo) {
                    // It's possible this shard was removed
                    Shard::reloadShardInfo();
                    needsToReloadShardInfo = false;
                }

                scoped_ptr<ScopedDbConnection> conn(
                        ScopedDbConnection::getInternalScopedDbConnection( _addr ) );

                BSONObj result;

                {
                    BSONObjBuilder cmd;
                    cmd.appendOID( "writebacklisten" , &serverID ); // Command will block for data
                    if ( ! conn->get()->runCommand( "admin" , cmd.obj() , result ) ) {
                        result = result.getOwned();
                        log() <<  "writebacklisten command failed!  "  << result << endl;
                        conn->done();
                        continue;
                    }

                }
                conn->done();

                LOG(1) << "writebacklisten result: " << result << endl;

                BSONObj data = result.getObjectField( "data" );
                if ( data.getBoolField( "writeBack" ) ) {
                    string ns = data["ns"].valuestrsafe();

                    ConnectionIdent cid( "" , 0 );
                    OID wid;
                    if ( data["connectionId"].isNumber() && data["id"].type() == jstOID ) {
                        string s = "";
                        if ( data["instanceIdent"].type() == String )
                            s = data["instanceIdent"].String();
                        cid = ConnectionIdent( s , data["connectionId"].numberLong() );
                        wid = data["id"].OID();
                    }
                    else {
                        warning() << "mongos/mongod version mismatch (1.7.5 is the split)" << endl;
                    }

                    int len; // not used, but needed for next call
                    Message msg( (void*)data["msg"].binData( len ) , false );
                    massert( 10427 ,  "invalid writeback message" , msg.header()->valid() );

                    DBConfigPtr db = grid.getDBConfig( ns );
                    ShardChunkVersion needVersion = ShardChunkVersion::fromBSON( data, "version" );

                    //
                    // TODO: Refactor the sharded strategy to correctly handle all sharding state changes itself,
                    // we can't rely on WBL to do this for us b/c anything could reset our state in-between.
                    // We should always reload here for efficiency when possible, but staleness is also caught in the
                    // loop below.
                    //

                    ChunkManagerPtr manager;
                    ShardPtr primary;
                    db->getChunkManagerOrPrimary( ns, manager, primary );

                    ShardChunkVersion currVersion;
                    if( manager ) currVersion = manager->getVersion();

                    LOG(1) << "connectionId: " << cid << " writebackId: " << wid << " needVersion : " << needVersion.toString()
                           << " mine : " << currVersion.toString() << endl;

                    LOG(1) << msg.toString() << endl;

                    //
                    // We should reload only if we need to update our version to be compatible *and* we
                    // haven't already done so.  This avoids lots of reloading when we remove/add a sharded collection
                    //

                    bool alreadyReloaded = lastNeededVersion &&
                                           lastNeededVersion->isEquivalentTo( needVersion );

                    if( alreadyReloaded ){

                        LOG(1) << "wbl already reloaded config information for version "
                               << needVersion << ", at version " << currVersion << endl;
                    }
                    else if( lastNeededVersion ) {

                        log() << "new version change detected to " << needVersion.toString()
                              << ", " << lastNeededCount << " writebacks processed at "
                              << lastNeededVersion->toString() << endl;

                        lastNeededCount = 0;
                    }

                    //
                    // Set our lastNeededVersion for next time
                    //

                    lastNeededVersion.reset( new ShardChunkVersion( needVersion ) );
                    lastNeededCount++;

                    //
                    // Determine if we should reload, if so, reload
                    //

                    bool shouldReload = ! needVersion.isWriteCompatibleWith( currVersion ) &&
                                        ! alreadyReloaded;

                    if( shouldReload && currVersion.isSet()
                                     && needVersion.isSet()
                                     && currVersion.hasCompatibleEpoch( needVersion ) )
                    {

                        //
                        // If we disagree about versions only, reload the chunk manager
                        //

                        db->getChunkManagerIfExists( ns, true );
                    }
                    else if( shouldReload ){

                        //
                        // If we disagree about anything else, reload the full db
                        //

                        warning() << "reloading config data for " << db->getName() << ", "
                                  << "wanted version " << needVersion.toString()
                                  << " but currently have version " << currVersion.toString() << endl;

                        db->reload();
                    }

                    // do request and then call getLastError
                    // we have to call getLastError so we can return the right fields to the user if they decide to call getLastError

                    BSONObj gle;
                    int attempts = 0;
                    while ( true ) {
                        attempts++;

                        try {

                            Request r( msg , 0 );
                            r.init();

                            r.d().reservedField() |= Reserved_FromWriteback;

                            ClientInfo * ci = r.getClientInfo();
                            if (!noauth) {
                                // TODO: Figure out why this is 'admin' instead of 'local'.
                                ci->getAuthenticationInfo()->authorize("admin", internalSecurity.user);
                                ci->getAuthorizationManager()->grantInternalAuthorization(
                                        "_writebackListener");
                            }
                            ci->noAutoSplit();

                            r.process( attempts );

                            ci->newRequest(); // this so we flip prev and cur shards

                            BSONObjBuilder b;
                            string errmsg;
                            if ( ! ci->getLastError( "admin",
                                                     BSON( "getLastError" << 1 ),
                                                     b,
                                                     errmsg,
                                                     true ) )
                            {
                                b.appendBool( "commandFailed" , true );
                                if( ! b.hasField( "errmsg" ) ){

                                    b.append( "errmsg", errmsg );
                                    gle = b.obj();
                                }
                                else if( errmsg.size() > 0 ){

                                    // Rebuild GLE object with errmsg
                                    // TODO: Make this less clumsy by improving GLE interface
                                    gle = b.obj();

                                    if( gle["errmsg"].type() == String ){

                                        BSONObj gleNoErrmsg =
                                                gle.filterFieldsUndotted( BSON( "errmsg" << 1 ),
                                                                          false );
                                        BSONObjBuilder bb;
                                        bb.appendElements( gleNoErrmsg );
                                        bb.append( "errmsg", gle["errmsg"].String() +
                                                             " ::and:: " +
                                                             errmsg );
                                        gle = bb.obj().getOwned();
                                    }
                                }
                            }
                            else{
                                gle = b.obj();
                            }

                            if ( gle["code"].numberInt() == 9517 ) {

                                log() << "new version change detected, "
                                      << lastNeededCount << " writebacks processed previously" << endl;

                                lastNeededVersion.reset();
                                lastNeededCount = 1;

                                log() << "writeback failed because of stale config, retrying attempts: " << attempts << endl;
                                LOG(1) << "writeback error : " << gle << endl;

                                //
                                // Bringing this in line with the similar retry logic elsewhere
                                //
                                // TODO: Reloading the chunk manager may not help if we dropped a
                                // collection, but we don't actually have that info in the writeback
                                // error
                                //

                                if( attempts <= 2 ){
                                    db->getChunkManagerIfExists( ns, true );
                                }
                                else{
                                    versionManager.forceRemoteCheckShardVersionCB( ns );
                                    sleepsecs( attempts - 1 );
                                }

                                uassert( 15884, str::stream()
                                         << "Could not reload chunk manager after "
                                         << attempts << " attempts.", attempts <= 4 );

                                continue;
                            }

                            ci->clearSinceLastGetError();
                        }
                        catch ( DBException& e ) {
                            error() << "error processing writeback: " << e << endl;
                            BSONObjBuilder b;
                            e.getInfo().append( b, "err", "code" );
                            gle = b.obj();
                        }

                        break;
                    }

                    {
                        scoped_lock lk( _seenWritebacksLock );
                        WBStatus& s = _seenWritebacks[cid];
                        s.id = wid;
                        s.gle = gle;
                    }
                }
                else if ( result["noop"].trueValue() ) {
                    // no-op
                }
                else {
                    log() << "unknown writeBack result: " << result << endl;
                }

                secsToSleep = 0;
                continue;
            }
            catch ( std::exception& e ) {
                // Attention! Do not call any method that would throw an exception
                // (or assert) in this block.

                if ( inShutdown() ) {
                    // we're shutting down, so just clean up
                    return;
                }

                log() << "WriteBackListener exception : " << e.what() << endl;

                needsToReloadShardInfo = true;
            }
            catch ( ... ) {
                log() << "WriteBackListener uncaught exception!" << endl;
            }
            secsToSleep++;
            sleepsecs(secsToSleep);
            if ( secsToSleep > 10 )
                secsToSleep = 0;
        }

        log() << "WriteBackListener exiting : address no longer in cluster " << _addr;

    }

}  // namespace mongo
