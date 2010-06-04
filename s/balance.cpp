// balance.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "../db/jsobj.h"
#include "../db/cmdline.h"

#include "balance.h"
#include "server.h"
#include "shard.h"
#include "config.h"
#include "chunk.h"

namespace mongo {
    
    Balancer balancer;

    Balancer::Balancer() : _policy( new BalancerPolicy ){}

    Balancer::~Balancer() {
        delete _policy;
    }

    bool Balancer::_shouldIBalance( DBClientBase& conn ){
        BSONObj x = conn.findOne( ShardNS::settings , BSON( "_id" << "balancer" ) );
        log(2) << "balancer: " << x << endl;
        
        if ( ! x.isEmpty() ){
            if ( x["who"].String() == _myid ){
                log(2) << "balancer: i'm the current balancer" << endl;
                return true;
            }
            
            BSONObj other = conn.findOne( ShardNS::mongos , x["who"].wrap( "_id" ) );
            massert( 13125 , (string)"can't find mongos: " + x["who"].String() , ! other.isEmpty() );

            int secsSincePing = (int)(( jsTime() - other["ping"].Date() ) / 1000 );
            log(2) << "current balancer is: " << other << " ping delay(secs): " << secsSincePing << endl;
            
            if ( secsSincePing < ( 60 * 10 ) ){
                return false;
            }
            
            log() << "balancer: going to take over" << endl;
            // we want to take over, so fall through to below
        }

        // Taking over means replacing 'who' with this balancer's address. Note that
        // to avoid any races, we use a compare-and-set strategy relying on the 
        // incarnation of the previous balancer (the key 'x').

        OID incarnation;
        incarnation.init();
        
        BSONObjBuilder updateQuery;
        updateQuery.append( "_id" , "balancer" );
        if ( x["x"].type() )
            updateQuery.append( x["x"] );
        else
            updateQuery.append( "x" , BSON( "$exists" << false ) );
        
        conn.update( ShardNS::settings , 
                     updateQuery.obj() ,
                     BSON( "$set" << BSON( "who" << _myid << "x" << incarnation ) ) ,
                     true );

        // If another balancer beats this one to the punch, the following query will see 
        // the incarnation for that other guy.
        
        x = conn.findOne( ShardNS::settings , BSON( "_id" << "balancer" ) );
        log() << "balancer: after update: " << x << endl;
        return _myid == x["who"].String() && incarnation == x["x"].OID();
    }    

    int Balancer::_moveChunks( const vector<BalancerPolicy::ChunkInfoPtr>* toBalance ) {
        int movedCount = 0;

        for ( vector<BalancerPolicy::ChunkInfoPtr>::const_iterator it = toBalance->begin(); it != toBalance->end(); ++it ){
            const BalancerPolicy::ChunkInfo& chunkInfo = *it->get();

            DBConfigPtr cfg = grid.getDBConfig( chunkInfo.ns );
            assert( cfg );
        
            ChunkManagerPtr cm = cfg->getChunkManager( chunkInfo.ns );
            assert( cm );
        
            const BSONObj& chunkToMove = chunkInfo.chunk;
            ChunkPtr c = cm->findChunk( chunkToMove["min"].Obj() );
            if ( c->getMin().woCompare( chunkToMove["min"].Obj() ) ){
                // likely a split happened somewhere
                cm = cfg->getChunkManager( chunkInfo.ns , true );
                assert( cm );

                c = cm->findChunk( chunkToMove["min"].Obj() );
                if ( c->getMin().woCompare( chunkToMove["min"].Obj() ) ){
                    log() << "balancer: chunk mismatch after reload, ignoring will retry issue cm: " 
                          << c->getMin() << " min: " << chunkToMove["min"].Obj() << endl;
                    continue;
                }
            }
        
            string errmsg;
            if ( c->moveAndCommit( Shard::make( chunkInfo.to ) , errmsg ) ){
                movedCount++;
                continue;
            }

            log() << "balancer: MOVE FAILED **** " << errmsg << "\n"
                  << "  from: " << chunkInfo.from << " to: " << " chunk: " << chunkToMove << endl;
        }

        return movedCount;
    }
    
    void Balancer::_ping(){
        assert( _myid.size() && _started );
        try {
            ScopedDbConnection conn( configServer.getPrimary() );
            _ping( conn.conn() );
            conn.done();
        }
        catch ( std::exception& e ){
            log() << "bare ping failed: " << e.what() << endl;
        }
        
    }

    void Balancer::_ping( DBClientBase& conn ){
        WriteConcern w = conn.getWriteConcern();
        conn.setWriteConcern( W_NONE );

        conn.update( ShardNS::mongos , 
                      BSON( "_id" << _myid ) , 
                      BSON( "$set" << BSON( "ping" << DATENOW << "up" << (int)(time(0)-_started) ) ) , 
                      true );

        conn.setWriteConcern( w);
    }
    
    bool Balancer::_checkOIDs(){
        vector<Shard> all;
        Shard::getAllShards( all );
        
        map<int,Shard> oids;
        
        for ( vector<Shard>::iterator i=all.begin(); i!=all.end(); ++i ){
            Shard s = *i;
            BSONObj f = s.runCommand( "admin" , "features" );
            if ( f["oidMachine"].isNumber() ){
                int x = f["oidMachine"].numberInt();
                if ( oids.count(x) == 0 ){
                    oids[x] = s;
                }
                else {
                    log() << "error: 2 machines have " << x << " as oid machine piece " << s.toString() << " and " << oids[x].toString() << endl;
                    s.runCommand( "admin" , BSON( "features" << 1 << "oidReset" << 1 ) );
                    oids[x].runCommand( "admin" , BSON( "features" << 1 << "oidReset" << 1 ) );
                    return false;
                }
            }
            else {
                log() << "warning: oidMachine not set on: " << s.toString() << endl;
            }
        }
        return true;
    }

    void Balancer::run(){

        { // init stuff, don't want to do at static init
            StringBuilder buf;
            buf << ourHostname << ":" << cmdLine.port;
            _myid = buf.str();
            log(1) << "balancer myid: " << _myid << endl;
            
            _started = time(0);
        }
        
        _ping();
        _checkOIDs();

        while ( ! inShutdown() ){
            sleepsecs( 10 );
            
            try {
                ScopedDbConnection conn( configServer.getPrimary() );
                _ping( conn.conn() );
                
                if ( ! _checkOIDs() ){
                    uassert( 13258 , "oids broken after resetting!" , _checkOIDs() );
                }
                                    
                vector<BalancerPolicy::ChunkInfoPtr> toBalance;
                if ( _shouldIBalance( conn.conn() ) ){
                    _policy->balance( conn.conn(), &toBalance );
                    if ( toBalance.size() > 0 ) {
                        int moves = _moveChunks( &toBalance );
                        _policy->setBalancedLastTime( moves );
                    }
                }
                
                conn.done();
            }
            catch ( std::exception& e ){
                log() << "caught exception while doing balance: " << e.what() << endl;
                continue;
            }
            
        }
    }

}  // namespace mongo
