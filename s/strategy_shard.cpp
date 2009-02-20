// strategy_sharded.cpp

#include "stdafx.h"
#include "request.h"
#include "shard.h"
#include "../client/connpool.h"
#include "../db/commands.h"

namespace mongo {

    class ShardStrategy : public Strategy {

        virtual void queryOp( Request& r ){
            throw UserException( "shard query doesn't work" );
            QueryMessage q( r.d() );
            
            bool lateAssert = false;
        
            log(3) << "query: " << q.ns << "  " << q.query << endl;

            try {
                if ( q.ntoreturn == 1 && strstr(q.ns, ".$cmd") )
                    throw UserException( "something is wrong, shouldn't see a command here" );
                
                ScopedDbConnection dbcon( r.singleServerName() );
                DBClientBase &_c = dbcon.conn();
                
                // TODO: This will not work with Paired connections.  Fix. 
                DBClientConnection&c = dynamic_cast<DBClientConnection&>(_c);
                Message response;
                bool ok = c.port().call( r.m(), response);
                uassert("mongos: error calling db", ok);
                lateAssert = true;
                r.reply( response  );
                dbcon.done();
            }
            catch ( AssertionException& e ) {
                assert( !lateAssert );
                BSONObjBuilder err;
                err.append("$err", string("mongos: ") + (e.msg.empty() ? "assertion during query" : e.msg));
                BSONObj errObj = err.done();
                replyToQuery(QueryResult::ResultFlag_ErrSet, r.p() , r.m() , errObj);
                return;
            }

        }
        
        virtual void getMore( Request& r ){
            throw UserException( "shard getMore doesn't work yet" );
        }
        
        virtual void writeOp( int op , Request& r ){
            
            const char *ns = r.getns();
            log(3) << "write: " << ns << endl;
            
            DbMessage& d = r.d();
            ShardInfo * info = r.getShardInfo();
            assert( info );
            
            if ( op == dbInsert ){
                while ( d.moreJSObjs() ){
                    BSONObj o = d.nextJsObj();
                    if ( ! info->hasShardKey( o ) ){
                        log() << "tried to insert object without shard key: " << ns << "  " << o << endl;
                        throw UserException( "tried to insert object without shard key" );
                    }
                    
                    Shard& s = info->findShard( o );
                    log(4) << "  server:" << s.getServer() << " " << o << endl;
                    insert( s.getServer() , ns , o );
                }
            }
            else if ( op == dbUpdate ){
                throw UserException( "can't do update yet on sharded collection" );
            }
            else {
                log() << "sharding can't do write op: " << op << endl;
                throw UserException( "can't do this write op on sharded collection" );
            }
            
        }
    };
    
    Strategy * SHARDED = new ShardStrategy();
}
