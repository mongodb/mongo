// strategy_sharded.cpp

#include "stdafx.h"
#include "request.h"
#include "shard.h"
#include "cursors.h"
#include "../client/connpool.h"
#include "../db/commands.h"

namespace mongo {
    
    class ShardStrategy : public Strategy {

        virtual void queryOp( Request& r ){
            QueryMessage q( r.d() );
            
            log(3) << "shard query: " << q.ns << "  " << q.query << endl;
            
            if ( q.ntoreturn == 1 && strstr(q.ns, ".$cmd") )
                throw UserException( "something is wrong, shouldn't see a command here" );

            ShardManager * info = r.getShardManager();
            assert( info );
            
            Query query( q.query );

            vector<Shard*> shards;
            info->getShardsForQuery( shards , query.getFilter()  );
            
            set<ServerAndQuery> servers;
            map<string,int> serverCounts;
            for ( vector<Shard*>::iterator i = shards.begin(); i != shards.end(); i++ ){
                servers.insert( (*i)->getServer() );
                int& num = serverCounts[(*i)->getServer()];
                num++;
            }
            
            if ( servers.size() == 1 ){
                doQuery( r , servers.begin()->_server );
                return;
            }
            
            ShardedCursor * cursor = 0;
            
            BSONObj sort = query.getSort();
            
            if ( sort.isEmpty() ){
                // 1. no sort, can just hit them in serial
                cursor = new SerialServerShardedCursor( servers , q );
            }
            else {
                int shardKeyOrder = info->getShardKey().canOrder( sort );
                if ( shardKeyOrder ){
                    // 2. sort on shard key, can do in serial intelligently
                    set<ServerAndQuery> buckets;
                    for ( vector<Shard*>::iterator i = shards.begin(); i != shards.end(); i++ ){
                        Shard * s = *i;
                        BSONObj extra = emptyObj;
                        if ( serverCounts[s->getServer()] > 1 ){
                            BSONObjBuilder b;
                            s->getFilter( b );
                            extra = b.obj();
                        }
                        buckets.insert( ServerAndQuery( s->getServer() , extra , s->getMin() ) );
                    }
                    cursor = new SerialServerShardedCursor( buckets , q , shardKeyOrder );
                }
                else {
                    // 3. sort on non-sharded key, pull back a portion from each server and iterate slowly
                    cursor = new ParallelSortShardedCursor( servers , q , sort );
                }
            }

            assert( cursor );
            if ( ! cursor->sendNextBatch( r ) ){
                delete( cursor );
                return;
            }
            log(6) << "storing cursor : " << cursor->getId() << endl;
            cursorCache.store( cursor );
        }
        
        virtual void getMore( Request& r ){
            int ntoreturn = r.d().pullInt();
            long long id = r.d().pullInt64();

            log(6) << "want cursor : " << id << endl;

            ShardedCursor * cursor = cursorCache.get( id );
            if ( ! cursor ){
                log(6) << "\t invalid cursor :(" << endl;
                replyToQuery( QueryResult::ResultFlag_CursorNotFound , r.p() , r.m() , 0 , 0 , 0 );
                return;
            }
            
            if ( cursor->sendNextBatch( r , ntoreturn ) ){
                log(6) << "\t cursor finished: " << id << endl;
                return;
            }
            
            delete( cursor );
            cursorCache.remove( id );
        }
        
        virtual void writeOp( int op , Request& r ){
            
            const char *ns = r.getns();
            log(3) << "write: " << ns << endl;
            
            DbMessage& d = r.d();
            ShardManager * info = r.getShardManager();
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
