/* dbgrid/request.cpp

   Top level handling of requests (operations such as query, insert, ...)
*/

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

#include "stdafx.h"
#include "server.h"
#include "../db/commands.h"
#include "../db/dbmessage.h"
#include "../client/connpool.h"

#include "request.h"
#include "config.h"
#include "shard.h"

namespace mongo {

    Request::Request( Message& m, AbstractMessagingPort* p ) : _m(m) , _d( m ) , _p(p){
        assert( _d.getns() );
        _id = _m.data->id;
        
        reset();
    }

    void Request::reset( bool reload ){
        _config = grid.getDBConfig( getns() );

        if ( _config->sharded( getns() ) ){
            _shardInfo = _config->getShardManager( getns() , reload );
            uassert( (string)"no shard info for: " + getns() , _shardInfo );
        }
        else {
            _shardInfo = 0;
        }        

        _m.data->id = _id;
        
    }
    
    string Request::singleServerName(){
        if ( _shardInfo ){
            if ( _shardInfo->numShards() > 1 )
                throw UserException( "can't call singleServerName on a sharded collection" );
            return _shardInfo->findShard( _shardInfo->getShardKey().globalMin() ).getServer();
        }
        string s = _config->getServer( getns() );
        uassert( "can't call singleServerName on a sharded collection!" , s.size() > 0 );
        return s;
    }
    
    void Request::process( int attempt ){

        log(2) << "Request::process ns: " << getns() << " msg id:" << (int)(_m.data->id) << " attempt: " << attempt << endl;

        int op = _m.data->operation();
        assert( op > dbMsg );
        
        Strategy * s = SINGLE;
        
        _d.markSet();
        if ( getConfig()->isPartitioned() && op == dbQuery ){
            // there are a few things we need to check here
            // 1. db.eval
            //     TODO:  right now i'm just going to block all
            //            will need to make it look at function later
            // 2. $where - can't access DB
            //              TODO: make it smarter
            //cerr << "E1.b" << endl;
            QueryMessage q( _d );
            BSONObj query = q.query;
            
            if ( q.ntoreturn == 1 && 
                 strstr( q.ns , ".$cmd" ) &&
                 strcmp( "$eval" , query.firstElement().fieldName() ) == 0 ){
                log() << "trying to eval: " << q.query << endl;
                throw UserException( "eval not supported on partitioned databases yet" );
            }
            
            if ( query.hasField( "$where" ) )
                throw UserException( "$where not supported for partitioned databases yet" );

            _d.markReset();
        }

        if ( _shardInfo ){
            //if ( _shardInfo->numShards() > 1 )
                s = SHARDED;
        }

        if ( op == dbQuery ) {
            try {
                s->queryOp( *this );
            }
            catch ( StaleConfigException& staleConfig ){
                log() << staleConfig.what() << " attempt: " << attempt << endl;
                uassert( "too many attempts to update config, failing" , attempt < 5 );
                
                sleepsecs( attempt );
                reset( true );
                _d.markReset();
                process( attempt + 1 );
                return;
            }
        }
        else if ( op == dbGetMore ) {
            s->getMore( *this );
        }
        else {
            s->writeOp( op, *this );
        }
    }
    
} // namespace mongo
