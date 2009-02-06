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

/* TODO
   _ GridD


   _ concurrency control.
     _ connection pool
     _ hostbyname_nonreentrant() problem
   _ gridconfig object which gets config from the grid db.
     connect to iad-sb-grid
   _ limit() works right?
   _ KillCursors

   later
   _ secondary indexes
*/

#include "stdafx.h"
#include "server.h"
#include "../db/commands.h"
#include "../db/dbmessage.h"
#include "../client/connpool.h"

#include "request.h"
#include "gridconfig.h"

namespace mongo {

    void processRequest(Message& m, MessagingPort& p) {
        Request r( m , p );

        int op = m.data->operation();
        assert( op > dbMsg );
        
        grid.getDBConfig( r.getns() );
        
        if ( op == dbQuery ) {
            SINGLE->queryOp( r );
        }
        else if ( op == dbGetMore ) {
            SINGLE->getMore( r );
        }
        else {
            SINGLE->writeOp( op, r );
        }
    }
    
} // namespace mongo
