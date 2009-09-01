// strategy_sharded.cpp

#include "stdafx.h"
#include "request.h"
#include "chunk.h"
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

            ChunkManager * info = r.getChunkManager();
            assert( info );
            
            Query query( q.query );

            vector<Chunk*> shards;
            info->getChunksForQuery( shards , query.getFilter()  );
            
            set<ServerAndQuery> servers;
            map<string,int> serverCounts;
            for ( vector<Chunk*>::iterator i = shards.begin(); i != shards.end(); i++ ){
                servers.insert( (*i)->getShard() );
                int& num = serverCounts[(*i)->getShard()];
                num++;
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
                    for ( vector<Chunk*>::iterator i = shards.begin(); i != shards.end(); i++ ){
                        Chunk * s = *i;
                        BSONObj extra = BSONObj();
                        if ( serverCounts[s->getShard()] > 1 ){
                            BSONObjBuilder b;
                            s->getFilter( b );
                            extra = b.obj();
                            cout << s->toString() << " -->> " << extra << endl;
                        }
                        buckets.insert( ServerAndQuery( s->getShard() , extra , s->getMin() ) );
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
        
        void _insert( Request& r , DbMessage& d, ChunkManager* manager ){
            
            while ( d.moreJSObjs() ){
                BSONObj o = d.nextJsObj();
                if ( ! manager->hasShardKey( o ) ){
                    log() << "tried to insert object without shard key: " << r.getns() << "  " << o << endl;
                    throw UserException( "tried to insert object without shard key" );
                }
                
                Chunk& c = manager->findChunk( o );
                log(4) << "  server:" << c.getShard() << " " << o << endl;
                insert( c.getShard() , r.getns() , o );
                
                c.splitIfShould( o.objsize() );
            }            
        }

        void _update( Request& r , DbMessage& d, ChunkManager* manager ){
            int flags = d.pullInt();
            
            BSONObj query = d.nextJsObj();
            uassert( "invalid update" , d.moreJSObjs() );
            BSONObj toupdate = d.nextJsObj();

            
            bool upsert = flags & 1;
            if ( upsert && ! manager->hasShardKey( toupdate ) )
                throw UserException( "can't upsert something without shard key" );

            bool save = false;
            if ( ! manager->hasShardKey( query ) ){
                if ( query.nFields() != 1 || strcmp( query.firstElement().fieldName() , "_id" ) )
                    throw UserException( "can't do update with query that doesn't have the shard key" );
                save = true;
            }
            
            if ( ! save && manager->hasShardKey( toupdate ) && manager->getShardKey().compare( query , toupdate ) ){
                throw UserException( "change would move shards!" );
            }

            Chunk& c = manager->findChunk( toupdate );
            doWrite( dbUpdate , r , c.getShard() );

            c.splitIfShould( d.msg().data->dataLen() );
        }
        
        void _delete( Request& r , DbMessage& d, ChunkManager* manager ){

            int flags = d.pullInt();
            bool justOne = flags & 1;
            
            uassert( "bad delete message" , d.moreJSObjs() );
            BSONObj pattern = d.nextJsObj();
            
            if ( manager->hasShardKey( pattern ) ){
                Chunk& c = manager->findChunk( pattern );
                doWrite( dbDelete , r , c.getShard() );
                return;
            }
            
            if ( ! justOne && ! pattern.hasField( "_id" ) )
                throw UserException( "can only delete with a non-shard key pattern if can delete as many as we find" );
            
            vector<Chunk*> chunks;
            manager->getChunksForQuery( chunks , pattern );
            
            set<string> seen;
            for ( vector<Chunk*>::iterator i=chunks.begin(); i!=chunks.end(); i++){
                Chunk * c = *i;
                if ( seen.count( c->getShard() ) )
                    continue;
                seen.insert( c->getShard() );
                doWrite( dbDelete , r , c->getShard() );
            }
        }
        
        virtual void writeOp( int op , Request& r ){
            const char *ns = r.getns();
            log(3) << "write: " << ns << endl;
            
            DbMessage& d = r.d();
            ChunkManager * info = r.getChunkManager();
            assert( info );
            
            if ( op == dbInsert ){
                _insert( r , d , info );
            }
            else if ( op == dbUpdate ){
                _update( r , d , info );    
            }
            else if ( op == dbDelete ){
                _delete( r , d , info );
            }
            else {
                log() << "sharding can't do write op: " << op << endl;
                throw UserException( "can't do this write op on sharded collection" );
            }
            
        }
    };
    
    Strategy * SHARDED = new ShardStrategy();
}
