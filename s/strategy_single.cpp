// strategy_simple.cpp

#include "stdafx.h"
#include "request.h"
#include "../client/connpool.h"
#include "../db/commands.h"

namespace mongo {

    class SingleStrategy : public Strategy {
        
    public:
        SingleStrategy(){
            _commandsSafeToPass.insert( "$eval" );
            _commandsSafeToPass.insert( "create" );
        }

    private:
        virtual void queryOp( Request& r ){
            QueryMessage q( r.d() );
            
            bool lateAssert = false;
        
            log(3) << "single query: " << q.ns << "  " << q.query << "  ntoreturn: " << q.ntoreturn << endl;
            
            try {
                if ( ( q.ntoreturn == -1 || q.ntoreturn == 1 ) && strstr(q.ns, ".$cmd") ) {
                    BSONObjBuilder builder;
                    bool ok = runCommandAgainstRegistered(q.ns, q.query, builder);
                    if ( ok ) {
                        BSONObj x = builder.done();
                        replyToQuery(0, r.p(), r.m(), x);
                        return;
                    }
                    
                    string commandName = q.query.firstElement().fieldName();
                    if (  ! _commandsSafeToPass.count( commandName ) )
                        log() << "passing through unknown command: " << commandName << " " << q.query << endl;
                }

                lateAssert = true;
                doQuery( r , r.singleServerName() );
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
            const char *ns = r.getns();
        
            log(3) << "single getmore: " << ns << endl;

            ScopedDbConnection dbcon( r.singleServerName() );
            DBClientBase& _c = dbcon.conn();

            // TODO 
            DBClientConnection &c = dynamic_cast<DBClientConnection&>(_c);

            Message response;
            bool ok = c.port().call( r.m() , response);
            uassert("dbgrid: getmore: error calling db", ok);
            r.reply( response );
        
            dbcon.done();

        }
        
        void handleIndexWrite( int op , Request& r ){
            
            DbMessage& d = r.d();

            if ( op == dbInsert ){
                while( d.moreJSObjs() ){
                    BSONObj o = d.nextJsObj();
                    const char * ns = o["ns"].valuestr();
                    if ( r.getConfig()->isSharded( ns ) ){
                        uassert( "can't use unique indexes with sharding" , ! o["unique"].trueValue() );
                        ChunkManager * cm = r.getConfig()->getChunkManager( ns );
                        assert( cm );
                        for ( int i=0; i<cm->numChunks();i++)
                            doWrite( op , r , cm->getChunk(i)->getShard() );
                    }
                    else {
                        doWrite( op , r , r.singleServerName() );
                    }
                }
            }
            else if ( op == dbUpdate ){
                throw UserException( "can't update system.indexes" );
            }
            else if ( op == dbDelete ){
                // TODO
                throw UserException( "can't delete indexes on sharded collection yet" );
            }
            else {
                log() << "handleIndexWrite invalid write op: " << op << endl;
                throw UserException( "handleIndexWrite invalid write op" );
            }
                    
        }

        virtual void writeOp( int op , Request& r ){
            const char *ns = r.getns();
            
            if ( r.isShardingEnabled() && 
                 strstr( ns , ".system.indexes" ) == strstr( ns , "." ) && 
                 strstr( ns , "." ) ){
                log(1) << " .system.indexes write for: " << ns << endl;
                handleIndexWrite( op , r );
                return;
            }
            
            log(3) << "single write: " << ns << endl;
            doWrite( op , r , r.singleServerName() );
        }

        set<string> _commandsSafeToPass;
    };
    
    Strategy * SINGLE = new SingleStrategy();
}
