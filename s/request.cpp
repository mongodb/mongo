// s/request.cpp

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
#include "server.h"

#include "../db/commands.h"
#include "../db/dbmessage.h"
#include "../db/stats/counters.h"

#include "../client/connpool.h"

#include "request.h"
#include "config.h"
#include "chunk.h"
#include "stats.h"
#include "cursors.h"
#include "grid.h"
#include "client.h"

namespace mongo {

    Request::Request( Message& m, AbstractMessagingPort* p ) :
        _m(m) , _d( m ) , _p(p) , _didInit(false) {

        assert( _d.getns() );
        _id = _m.header()->id;

        _clientId = p ? p->getClientId() : 0;
        _clientInfo = ClientInfo::get( _clientId );
        _clientInfo->newRequest( p );

    }

    void Request::init() {
        if ( _didInit )
            return;
        _didInit = true;
        reset();
    }

    void Request::reset( bool reload ) {
        if ( _m.operation() == dbKillCursors ) {
            return;
        }

        uassert( 13644 , "can't use 'local' database through mongos" , ! str::startsWith( getns() , "local." ) );

        _config = grid.getDBConfig( getns() );
        if ( reload )
            uassert( 10192 ,  "db config reload failed!" , _config->reload() );

        if ( _config->isSharded( getns() ) ) {
            _chunkManager = _config->getChunkManager( getns() , reload );
            uassert( 10193 ,  (string)"no shard info for: " + getns() , _chunkManager );
        }
        else {
            _chunkManager.reset();
        }

        _m.header()->id = _id;

    }

    Shard Request::primaryShard() const {
        assert( _didInit );

        if ( _chunkManager ) {
            if ( _chunkManager->numChunks() > 1 )
                throw UserException( 8060 , "can't call primaryShard on a sharded collection" );
            return _chunkManager->findChunk( _chunkManager->getShardKey().globalMin() )->getShard();
        }
        Shard s = _config->getShard( getns() );
        uassert( 10194 ,  "can't call primaryShard on a sharded collection!" , s.ok() );
        return s;
    }

    void Request::process( int attempt ) {
        init();
        int op = _m.operation();
        assert( op > dbMsg );

        if ( op == dbKillCursors ) {
            cursorCache.gotKillCursors( _m );
            return;
        }


        log(3) << "Request::process ns: " << getns() << " msg id:" << (int)(_m.header()->id) << " attempt: " << attempt << endl;

        Strategy * s = SINGLE;
        _counter = &opsNonSharded;

        _d.markSet();

        if ( _chunkManager ) {
            s = SHARDED;
            _counter = &opsSharded;
        }

        bool iscmd = false;
        if ( op == dbQuery ) {
            iscmd = isCommand();
            try {
                s->queryOp( *this );
            }
            catch ( StaleConfigException& staleConfig ) {
                log() << staleConfig.what() << " attempt: " << attempt << endl;
                uassert( 10195 ,  "too many attempts to update config, failing" , attempt < 5 );
                ShardConnection::checkMyConnectionVersions( getns() );
                if (!staleConfig.justConnection() )
                    sleepsecs( attempt );
                reset( ! staleConfig.justConnection() );
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

        globalOpCounters.gotOp( op , iscmd );
        _counter->gotOp( op , iscmd );
    }

    bool Request::isCommand() const {
        int x = _d.getQueryNToReturn();
        return ( x == 1 || x == -1 ) && strstr( getns() , ".$cmd" );
    }

    void Request::gotInsert() {
        globalOpCounters.gotInsert();
        _counter->gotInsert();
    }

    void Request::reply( Message & response , const string& fromServer ) {
        assert( _didInit );
        long long cursor =response.header()->getCursor();
        if ( cursor ) {
            if ( fromServer.size() ) {
                cursorCache.storeRef( fromServer , cursor );
            }
            else {
                // probably a getMore
                // make sure we have a ref for this
                assert( cursorCache.getRef( cursor ).size() );
            }
        }
        _p->reply( _m , response , _id );
    }

} // namespace mongo
