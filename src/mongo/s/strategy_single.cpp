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
#include "cursors.h"
#include "../client/connpool.h"
#include "../db/commands.h"

namespace mongo {

    class SingleStrategy : public Strategy {

    public:
        SingleStrategy() {
            _commandsSafeToPass.insert( "$eval" );
            _commandsSafeToPass.insert( "create" );
        }

    private:
        virtual void queryOp( Request& r ) {
            QueryMessage q( r.d() );

            LOG(3) << "single query: " << q.ns << "  " << q.query << "  ntoreturn: " << q.ntoreturn << " options : " << q.queryOptions << endl;

            if ( r.isCommand() ) {

                if ( handleSpecialNamespaces( r , q ) )
                    return;

                int loops = 5;
                while ( true ) {
                    BSONObjBuilder builder;
                    try {
                        BSONObj cmdObj = q.query;
                        {
                            BSONElement e = cmdObj.firstElement();
                            if ( e.type() == Object && (e.fieldName()[0] == '$'
                                                         ? str::equals("query", e.fieldName()+1)
                                                         : str::equals("query", e.fieldName())))
                                cmdObj = e.embeddedObject();
                        }
                        bool ok = Command::runAgainstRegistered(q.ns, cmdObj, builder, q.queryOptions);
                        if ( ok ) {
                            BSONObj x = builder.done();
                            replyToQuery(0, r.p(), r.m(), x);
                            return;
                        }
                        break;
                    }
                    catch ( StaleConfigException& e ) {
                        if ( loops <= 0 )
                            throw e;

                        loops--;
                        log() << "retrying command: " << q.query << endl;
                        ShardConnection::checkMyConnectionVersions( e.getns() );
                        if( loops < 4 ) versionManager.forceRemoteCheckShardVersionCB( e.getns() );
                    }
                    catch ( AssertionException& e ) {
                        e.getInfo().append( builder , "assertion" , "assertionCode" );
                        builder.append( "errmsg" , "db assertion failure" );
                        builder.append( "ok" , 0 );
                        BSONObj x = builder.done();
                        replyToQuery(0, r.p(), r.m(), x);
                        return;
                    }
                }

                string commandName = q.query.firstElementFieldName();

                uassert(13390, "unrecognized command: " + commandName, _commandsSafeToPass.count(commandName) != 0);
            }

            doQuery( r , r.primaryShard() );
        }

        virtual void getMore( Request& r ) {
            const char *ns = r.getns();

            LOG(3) << "single getmore: " << ns << endl;

            long long id = r.d().getInt64( 4 );
            
            // we used ScopedDbConnection because we don't get about config versions
            // not deleting data is handled elsewhere
            // and we don't want to call setShardVersion
            ScopedDbConnection conn( cursorCache.getRef( id ) );

            Message response;
            bool ok = conn->callRead( r.m() , response);
            uassert( 10204 , "dbgrid: getmore: error calling db", ok);
            r.reply( response , "" /*conn->getServerAddress() */ );

            conn.done();

        }

        // Deprecated
        virtual void writeOp( int op , Request& r ) {
            // Don't use anymore, requires single-step detection of chunk manager or primary
            verify( 0 );
        }

        bool handleSpecialNamespaces( Request& r , QueryMessage& q ) {
            const char * ns = r.getns();
            ns = strstr( r.getns() , ".$cmd.sys." );
            if ( ! ns )
                return false;
            ns += 10;

            r.checkAuth( Auth::WRITE );

            BSONObjBuilder b;
            vector<Shard> shards;

            if ( strcmp( ns , "inprog" ) == 0 ) {
                Shard::getAllShards( shards );

                BSONArrayBuilder arr( b.subarrayStart( "inprog" ) );

                for ( unsigned i=0; i<shards.size(); i++ ) {
                    Shard shard = shards[i];
                    ScopedDbConnection conn( shard );
                    BSONObj temp = conn->findOne( r.getns() , q.query );
                    if ( temp["inprog"].isABSONObj() ) {
                        BSONObjIterator i( temp["inprog"].Obj() );
                        while ( i.more() ) {
                            BSONObjBuilder x;

                            BSONObjIterator j( i.next().Obj() );
                            while( j.more() ) {
                                BSONElement e = j.next();
                                if ( str::equals( e.fieldName() , "opid" ) ) {
                                    stringstream ss;
                                    ss << shard.getName() << ':' << e.numberInt();
                                    x.append( "opid" , ss.str() );
                                }
                                else if ( str::equals( e.fieldName() , "client" ) ) {
                                    x.appendAs( e , "client_s" );
                                }
                                else {
                                    x.append( e );
                                }
                            }
                            arr.append( x.obj() );
                        }
                    }
                    conn.done();
                }

                arr.done();
            }
            else if ( strcmp( ns , "killop" ) == 0 ) {
                r.checkAuth( Auth::WRITE , "admin" );

                BSONElement e = q.query["op"];
                if ( e.type() != String ) {
                    b.append( "err" , "bad op" );
                    b.append( e );
                }
                else {
                    b.append( e );
                    string s = e.String();
                    string::size_type i = s.find( ':' );
                    if ( i == string::npos ) {
                        b.append( "err" , "bad opid" );
                    }
                    else {
                        string shard = s.substr( 0 , i );
                        int opid = atoi( s.substr( i + 1 ).c_str() );
                        b.append( "shard" , shard );
                        b.append( "shardid" , opid );

                        log() << "want to kill op: " << e << endl;
                        Shard s(shard);

                        ScopedDbConnection conn( s );
                        conn->findOne( r.getns() , BSON( "op" << opid ) );
                        conn.done();
                    }
                }
            }
            else if ( strcmp( ns , "unlock" ) == 0 ) {
                b.append( "err" , "can't do unlock through mongos" );
            }
            else {
                log( LL_WARNING ) << "unknown sys command [" << ns << "]" << endl;
                return false;
            }

            BSONObj x = b.done();
            replyToQuery(0, r.p(), r.m(), x);
            return true;
        }

        set<string> _commandsSafeToPass;
    };

    Strategy * SINGLE = new SingleStrategy();
}
