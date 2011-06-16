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
 */

#include "pch.h"

#include "../client/connpool.h"
#include "../db/commands.h"

#include "grid.h"
#include "request.h"
#include "server.h"
#include "writeback_listener.h"

#include "strategy.h"

namespace mongo {

    // ----- Strategy ------

    void Strategy::doWrite( int op , Request& r , const Shard& shard , bool checkVersion ) {
        ShardConnection conn( shard , r.getns() );
        if ( ! checkVersion )
            conn.donotCheckVersion();
        else if ( conn.setVersion() ) {
            conn.done();
            throw StaleConfigException( r.getns() , "doWRite" , true );
        }
        conn->say( r.m() );
        conn.done();
    }

    void Strategy::doQuery( Request& r , const Shard& shard ) {

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
                throw StaleConfigException( r.getns() , "Strategy::doQuery" );
            }
        }

        r.reply( response , actualServer.size() ? actualServer : c.getServerAddress() );
        dbcon.done();
    }

    void Strategy::insert( const Shard& shard , const char * ns , const BSONObj& obj , int flags ) {
        ShardConnection dbcon( shard , ns );
        if ( dbcon.setVersion() ) {
            dbcon.done();
            throw StaleConfigException( ns , "for insert" );
        }
        dbcon->insert( ns , obj , flags);
        dbcon.done();
    }

}
