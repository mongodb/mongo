// @file strategy.cpp

/*
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

#include "mongo/s/strategy.h"

#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/commands.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/request.h"
#include "mongo/s/server.h"
#include "mongo/s/stale_exception.h"  // for SendStaleConfigException
#include "mongo/s/writeback_listener.h"
#include "mongo/util/mongoutils/str.h"


namespace mongo {

    // ----- Strategy ------

    void Strategy::doWrite( int op , Request& r , const Shard& shard , bool checkVersion ) {

        // Now only used for index and broadcast writes
        // TODO: Remove

        ShardConnection conn( shard , r.getns() );
        if ( ! checkVersion )
            conn.donotCheckVersion();
        else if ( conn.setVersion() ) {
            conn.done();
            // Version is zero b/c we don't yet have a way to get the local version conflict
            throw RecvStaleConfigException( r.getns() , "doWrite" , ChunkVersion( 0, OID() ), ChunkVersion( 0, OID() ), true );
        }
        conn->say( r.m() );
        conn.done();
    }

    void Strategy::broadcastWrite(int op, Request& r){
        vector<Shard> shards;
        Shard::getAllShards(shards);
        for (vector<Shard>::iterator it(shards.begin()), end(shards.end()); it != end; ++it){
            doWrite(op, r, *it, false);
        }
    }


    void Strategy::doIndexQuery( Request& r , const Shard& shard ) {

        ShardConnection dbcon( shard , r.getns() );
        DBClientBase &c = dbcon.conn();

        string actualServer;

        Message response;
        bool ok = c.call( r.m(), response, true , &actualServer );
        uassert( 10200 , "mongos: error calling db", ok );

        {
            QueryResult *qr = (QueryResult *) response.singleData();
            if ( qr->resultFlags() & ResultFlag_ShardConfigStale ) {
                dbcon.done();
                // Version is zero b/c this is deprecated codepath
                throw RecvStaleConfigException( r.getns() , "Strategy::doQuery", ChunkVersion( 0, OID() ), ChunkVersion( 0, OID() ) );
            }
        }

        r.reply( response , actualServer.size() ? actualServer : c.getServerAddress() );
        dbcon.done();
    }

}
