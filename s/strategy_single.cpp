/*
 *    Copyright (C) 2010 10gen Inc.
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

// strategy_simple.cpp

#include "pch.h"
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
                if ( r.isCommand() ){
                    BSONObjBuilder builder;
                    bool ok = Command::runAgainstRegistered(q.ns, q.query, builder);
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
                doQuery( r , r.primaryShard() );
            }
            catch ( AssertionException& e ) {
                if ( lateAssert ){
                    log() << "lateAssert: " << e.msg << endl;
                    assert( !lateAssert );
                }

                BSONObjBuilder err;
                err.append("$err", string("mongos: ") + (e.msg.empty() ? "assertion during query" : e.msg));
                err.append("code",e.getCode());
                BSONObj errObj = err.done();
                replyToQuery(QueryResult::ResultFlag_ErrSet, r.p() , r.m() , errObj);
                return;
            }

        }
        
        virtual void getMore( Request& r ){
            const char *ns = r.getns();
        
            log(3) << "single getmore: " << ns << endl;

            ShardConnection dbcon( r.primaryShard() );
            DBClientBase& _c = dbcon.conn();

            // TODO 
            DBClientConnection &c = dynamic_cast<DBClientConnection&>(_c);

            Message response;
            bool ok = c.port().call( r.m() , response);
            uassert( 10204 , "dbgrid: getmore: error calling db", ok);
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
                        BSONObj newIndexKey = o["key"].embeddedObjectUserCheck();
                        
                        uassert( 10205 ,  (string)"can't use unique indexes with sharding  ns:" + ns + 
                                 " key: " + o["key"].embeddedObjectUserCheck().toString() , 
                                 IndexDetails::isIdIndexPattern( newIndexKey ) ||
                                 ! o["unique"].trueValue() || 
                                 r.getConfig()->getChunkManager( ns )->getShardKey().uniqueAllowd( newIndexKey ) );

                        ChunkManager * cm = r.getConfig()->getChunkManager( ns );
                        assert( cm );
                        for ( int i=0; i<cm->numChunks();i++)
                            doWrite( op , r , cm->getChunk(i)->getShard() );
                    }
                    else {
                        doWrite( op , r , r.primaryShard() );
                    }
                }
            }
            else if ( op == dbUpdate ){
                throw UserException( 8050 , "can't update system.indexes" );
            }
            else if ( op == dbDelete ){
                // TODO
                throw UserException( 8051 , "can't delete indexes on sharded collection yet" );
            }
            else {
                log() << "handleIndexWrite invalid write op: " << op << endl;
                throw UserException( 8052 , "handleIndexWrite invalid write op" );
            }
                    
        }

        virtual void writeOp( int op , Request& r ){
            const char *ns = r.getns();
            
            if ( r.isShardingEnabled() && 
                 strstr( ns , ".system.indexes" ) == strchr( ns , '.' ) && 
                 strchr( ns , '.' ) ) {
                log(1) << " .system.indexes write for: " << ns << endl;
                handleIndexWrite( op , r );
                return;
            }
            
            log(3) << "single write: " << ns << endl;
            doWrite( op , r , r.primaryShard() );
        }

        set<string> _commandsSafeToPass;
    };
    
    Strategy * SINGLE = new SingleStrategy();
}
