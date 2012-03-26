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

        verify( _d.getns() );
        _id = _m.header()->id;

        _clientInfo = ClientInfo::get();
        _clientInfo->newRequest( p );
    }

    void Request::checkAuth( Auth::Level levelNeeded , const char * need ) const {
        char cl[256];

        const char * use = cl;
        if ( need )
            use = need;
        else
            nsToDatabase(getns(), cl);
        
        uassert( 15845 , 
                 str::stream() << "unauthorized for db:" << use << " level: " << levelNeeded ,
                 _clientInfo->getAuthenticationInfo()->isAuthorizedForLevel(use,levelNeeded) );
    }

    void Request::init() {
        if ( _didInit )
            return;
        _didInit = true;
        reset();
    }

    // Deprecated, will move to the strategy itself
    void Request::reset() {
        if ( _m.operation() == dbKillCursors ) {
            return;
        }

        uassert( 13644 , "can't use 'local' database through mongos" , ! str::startsWith( getns() , "local." ) );

        // TODO: Deprecated, keeping to preserve codepath for now
        const string nsStr (getns()); // use in functions taking string rather than char*

        _config = grid.getDBConfig( nsStr );

        // TODO:  In general, throwing an exception when the cm doesn't exist is really annoying
        if ( _config->isSharded( nsStr ) ) {
            _chunkManager = _config->getChunkManagerIfExists( nsStr );
        }
        else {
            _chunkManager.reset();
        }

        _m.header()->id = _id;
        _clientInfo->clearCurrentShards();
    }

    // Deprecated, will move to the strategy itself
    Shard Request::primaryShard() const {
        verify( _didInit );

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
        verify( op > dbMsg );

        if ( op == dbKillCursors ) {
            cursorCache.gotKillCursors( _m );
            return;
        }


        LOG(3) << "Request::process ns: " << getns() << " msg id:" << (int)(_m.header()->id) << " attempt: " << attempt << endl;

        Strategy * s = SHARDED;
        _counter = &opsNonSharded;

        _d.markSet();

        bool iscmd = false;
        if ( op == dbQuery ) {
            iscmd = isCommand();
            s->queryOp( *this );
        }
        else if ( op == dbGetMore ) {
            checkAuth( Auth::READ ); // this is important so someone can't steal a cursor
            s->getMore( *this );
        }
        else {
            checkAuth( Auth::WRITE );
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
        verify( _didInit );
        long long cursor =response.header()->getCursor();
        if ( cursor ) {
            if ( fromServer.size() ) {
                cursorCache.storeRef( fromServer , cursor );
            }
            else {
                // probably a getMore
                // make sure we have a ref for this
                verify( cursorCache.getRef( cursor ).size() );
            }
        }
        _p->reply( _m , response , _id );
    }

} // namespace mongo
