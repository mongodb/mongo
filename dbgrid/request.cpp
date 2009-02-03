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
#include "../util/message.h"
#include "../db/commands.h"
#include "../db/dbmessage.h"
#include "../client/connpool.h"

namespace mongo {

    const char *tempHost = "localhost:27018";

    void getMore(Message& m, MessagingPort& p) {
        DbMessage d(m);
        const char *ns = d.getns();

        out() << "TEMP: getmore: " << ns << endl;

        ScopedDbConnection dbcon(tempHost);
        DBClientConnection &c = dbcon.conn();

        Message response;
        bool ok = c.port().call(m, response);
        uassert("dbgrid: getmore: error calling db", ok);
        p.reply(m, response, m.data->id);
        
        dbcon.done();
    }

    /* got query operation from a database */
    void queryOp(Message& m, MessagingPort& p) {
        const MSGID originalID = m.data->id;
        DbMessage d(m);
        QueryMessage q(d);
        bool lateAssert = false;
        try {
            if ( q.ntoreturn == -1 && strstr(q.ns, ".$cmd") ) {
                BSONObjBuilder builder;
                out() << q.query.toString() << endl;
                bool ok = runCommandAgainstRegistered(q.ns, q.query, builder);
                if ( ok ) {
                    BSONObj x = builder.done();
                    replyToQuery(0, p, m, x);
                    return;
                }
            }

            ScopedDbConnection dbcon(tempHost);
            DBClientConnection &c = dbcon.conn();
            Message response;
            bool ok = c.port().call(m, response);
            uassert("dbgrid: error calling db", ok);
            lateAssert = true;
            p.reply(m, response, originalID );
            dbcon.done();
        }
        catch ( AssertionException& e ) {
            assert( !lateAssert );
            BSONObjBuilder err;
            err.append("$err", string("dbgrid ") + (e.msg.empty() ? "dbgrid assertion during query" : e.msg));
            BSONObj errObj = err.done();
            replyToQuery(QueryResult::ResultFlag_ErrSet, p, m, errObj);
            return;
        }
    }
    
    void writeOp(int op, Message& m, MessagingPort& p) {
        DbMessage d(m);
        const char *ns = d.getns();
        assert( *ns );

        ScopedDbConnection dbcon(tempHost);
        DBClientConnection &c = dbcon.conn();

        c.port().say(m);

        dbcon.done();
        /*
          while( d.mmoreJSObjs() ) {
            BSONObj js = d.nextJsObj();
            const char *ns = d.getns();
            assert(*ns);
          }
        */
    }

    void processRequest(Message& m, MessagingPort& p) {
        int op = m.data->operation();
        assert( op > dbMsg );
        if ( op == dbQuery ) {
            queryOp(m,p);
        }
        else if ( op == dbGetMore ) {
            getMore(m,p);
        }
        else {
            writeOp(op, m, p);
        }
    }

} // namespace mongo
