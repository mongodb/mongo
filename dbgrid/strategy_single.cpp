// strategy_simple.cpp

#include "stdafx.h"
#include "request.h"
#include "../client/connpool.h"
#include "../db/commands.h"

namespace mongo {

    class SingleStrategy : public Strategy {

        virtual void queryOp( Request& r ){
            QueryMessage q( r.d() );
            
            bool lateAssert = false;
        
            log(3) << "query: " << q.ns << "  " << q.query << endl;

            try {
                if ( q.ntoreturn == 1 && strstr(q.ns, ".$cmd") ) {
                    BSONObjBuilder builder;
                    out() << q.query.toString() << endl;
                    bool ok = runCommandAgainstRegistered(q.ns, q.query, builder);
                    if ( ok ) {
                        BSONObj x = builder.done();
                        replyToQuery(0, r.p(), r.m(), x);
                        return;
                    }
                }

                ScopedDbConnection dbcon( r.primaryName() );
                DBClientConnection &c = dbcon.conn();
                Message response;
                bool ok = c.port().call( r.m(), response);
                uassert("dbgrid: error calling db", ok);
                lateAssert = true;
                r.reply( response  );
                dbcon.done();
            }
            catch ( AssertionException& e ) {
                assert( !lateAssert );
                BSONObjBuilder err;
                err.append("$err", string("dbgrid ") + (e.msg.empty() ? "dbgrid assertion during query" : e.msg));
                BSONObj errObj = err.done();
                replyToQuery(QueryResult::ResultFlag_ErrSet, r.p() , r.m() , errObj);
                return;
            }

        }
        
        virtual void getMore( Request& r ){
            const char *ns = r.getns();
        
            log(3) << "getmore: " << ns << endl;

            ScopedDbConnection dbcon( r.primaryName() );
            DBClientConnection &c = dbcon.conn();

            Message response;
            bool ok = c.port().call( r.m() , response);
            uassert("dbgrid: getmore: error calling db", ok);
            r.reply( response );
        
            dbcon.done();

        }
        
        virtual void writeOp( int op , Request& r ){
            const char *ns = r.getns();
            log(3) << "write: " << ns << endl;

            ScopedDbConnection dbcon( r.primaryName() );
            DBClientConnection &c = dbcon.conn();

            c.port().say( r.m() );

            dbcon.done();
            
        }
    };
    
    Strategy * SINGLE = new SingleStrategy();
}
