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

#include "mongo/pch.h"

#include "mongo/s/request.h"

#include "mongo/client/connpool.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/chunk.h"
#include "mongo/s/client_info.h"
#include "mongo/s/config.h"
#include "mongo/s/cursors.h"
#include "mongo/s/grid.h"
#include "mongo/s/server.h"

namespace mongo {

    Request::Request( Message& m, AbstractMessagingPort* p ) :
        _m(m) , _d( m ) , _p(p) , _didInit(false) {

        verify( _d.getns() );
        _id = _m.header()->id;

        _clientInfo = ClientInfo::get();
        if ( p ) {
            _clientInfo->newPeerRequest( p->remote() );
        }
        else {
            _clientInfo->newRequest();
        }
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
            return _chunkManager->findIntersectingChunk( _chunkManager->getShardKey().globalMin() )->getShard();
        }
        Shard s = _config->getShard( getns() );
        uassert( 10194 ,  "can't call primaryShard on a sharded collection!" , s.ok() );
        return s;
    }

    void Request::process( int attempt ) {
        init();
        int op = _m.operation();
        verify( op > dbMsg );

        int msgId = (int)(_m.header()->id);

        Timer t;
        LOG(3) << "Request::process begin ns: " << getns()
               << " msg id: " << msgId
               << " op: " << op
               << " attempt: " << attempt
               << endl;

        Strategy * s = SHARDED;

        _d.markSet();

        bool iscmd = false;
        if ( op == dbKillCursors ) {
            cursorCache.gotKillCursors( _m );
        }
        else if ( op == dbQuery ) {
            NamespaceString nss(getns());
            iscmd = nss.isCommand() || nss.isSpecialCommand();

            if (iscmd) {
                int n = _d.getQueryNToReturn();
                uassert( 16978, str::stream() << "bad numberToReturn (" << n
                                              << ") for $cmd type ns - can only be 1 or -1",
                         n == 1 || n == -1 );

                SINGLE->queryOp(*this);
            }
            else {
                s->queryOp( *this );
            }
        }
        else if ( op == dbGetMore ) {
            s->getMore( *this );
        }
        else {
            s->writeOp( op, *this );
        }

        LOG(3) << "Request::process end ns: " << getns()
               << " msg id: " << msgId
               << " op: " << op
               << " attempt: " << attempt
               << " " << t.millis() << "ms"
               << endl;

        globalOpCounters.gotOp( op , iscmd );
    }

    void Request::gotInsert() {
        globalOpCounters.gotInsert();
    }

    void Request::reply( Message & response , const string& fromServer ) {
        verify( _didInit );
        long long cursor =response.header()->getCursor();
        if ( cursor ) {
            if ( fromServer.size() ) {
                cursorCache.storeRef(fromServer, cursor, getns());
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
