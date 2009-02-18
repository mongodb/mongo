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

namespace mongo {

    Request::Request( Message& m, MessagingPort& p ) : _m(m) , _d( m ) , _p(p){
        assert( _d.getns() );
        _id = _m.data->id;
        _config = grid.getDBConfig( getns() );
    }

    void processRequest(Message& m, MessagingPort& p) {
        Request r( m , p );

        int op = m.data->operation();
        assert( op > dbMsg );
        
        Strategy * s = SINGLE;
        
        if ( r.getConfig()->isPartitioned() ){
            uassert( "partitioned not supported" , 0 );
        }

        if ( op == dbQuery ) {
            s->queryOp( r );
        }
        else if ( op == dbGetMore ) {
            s->getMore( r );
        }
        else {
            s->writeOp( op, r );
        }
    }
    
} // namespace mongo
