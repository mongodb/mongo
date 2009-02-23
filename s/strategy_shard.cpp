// strategy_sharded.cpp

#include "stdafx.h"
#include "request.h"
#include "shard.h"
#include "../client/connpool.h"
#include "../db/commands.h"

namespace mongo {
    
    class DownstreamServerState {
    public:
        DownstreamServerState( string name ) : _name( name ) , _used(0){ 
        }
        
        string _name;
        bool _used;
        long long _cursor;
    };

    class SerialServerShardedCursor : public ShardedCursor {
    public:
        SerialServerShardedCursor( set<string> servers , string ns , const BSONObj& q ){
            for ( set<string>::iterator i = servers.begin(); i!=servers.end(); i++ )
                _servers.push_back( DownstreamServerState( *i ) );

            _serverIndex = 0;
            
            _ns = ns;
            _query = q.copy();
        }

        virtual void sendNextBatch( Request& r ){
            throw UserException( "SerialServerShardedCursor doesn't work yet" );
        }

    private:
        vector<DownstreamServerState> _servers;
        int _serverIndex;
        
        string _ns;
        BSONObj _query;
    };
    
    class ShardStrategy : public Strategy {

        virtual void queryOp( Request& r ){
            QueryMessage q( r.d() );
            
            log(3) << "shard query: " << q.ns << "  " << q.query << endl;

            if ( q.ntoreturn == 1 && strstr(q.ns, ".$cmd") )
                throw UserException( "something is wrong, shouldn't see a command here" );

            ShardInfo * info = r.getShardInfo();
            assert( info );
            
            Query query( q.query );

            vector<Shard*> shards;
            info->getShardsForQuery( shards , query.getFilter()  );
            
            set<string> servers;
            for ( vector<Shard*>::iterator i = shards.begin(); i != shards.end(); i++ ){
                servers.insert( (*i)->getServer() );
            }
            
            if ( servers.size() == 1 ){
                doQuery( r , *(servers.begin()) );
                return;
            }
            
            SerialServerShardedCursor * cursor = 0;

            if ( query.getSort().isEmpty() ){
                // 1. no sort, can just hit them in serial
                cursor = new SerialServerShardedCursor( servers , q.ns , q.query );
            }
            else {
                // 2. sort on shard key, can do in serial intelligently
                // 3. sort on non-sharded key, pull back a portion from each server and iterate slowly

                throw UserException( "sorting and sharding doesn't work yet" );
            }
            
            assert( cursor );
            cursor->sendNextBatch( r );
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
