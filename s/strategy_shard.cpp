// strategy_sharded.cpp

#include "stdafx.h"
#include "request.h"
#include "shard.h"
#include "../client/connpool.h"
#include "../db/commands.h"

namespace mongo {

    class ShardStrategy : public Strategy {

        virtual void queryOp( Request& r ){
            QueryMessage q( r.d() );
            
            log(3) << "shard query: " << q.ns << "  " << q.query << endl;

            if ( q.ntoreturn == 1 && strstr(q.ns, ".$cmd") )
                throw UserException( "something is wrong, shouldn't see a command here" );

            ShardInfo * info = r.getShardInfo();
            assert( info );

            vector<Shard*> shards;
            if ( info->getShardsForQuery( shards , q.query ) == 1 ){
                doQuery( r , shards[0]->getServer() );
                return;
            }
            
            throw UserException( "real sharding doesn't nwork" );
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
