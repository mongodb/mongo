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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

// strategy_simple.cpp

#include "mongo/pch.h"

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/s/request.h"
#include "mongo/s/cursors.h"
#include "mongo/s/version_manager.h"

namespace mongo {

    class SingleStrategy : public Strategy {

    public:
        SingleStrategy() {}

    private:
        virtual void queryOp( Request& r ) {
            QueryMessage q( r.d() );

            LOG(3) << "single query: " << q.ns << "  " << q.query << "  ntoreturn: " << q.ntoreturn << " options : " << q.queryOptions << endl;

            NamespaceString nss( r.getns() );
            // Regular queries are handled in strategy_shard.cpp
            verify( nss.isCommand() || nss.isSpecialCommand() );

            if ( handleSpecialNamespaces( r , q ) )
                return;

            int loops = 5;
            while ( true ) {
                BSONObjBuilder builder;
                try {
                    BSONObj cmdObj = q.query;
                    {
                        BSONElement e = cmdObj.firstElement();
                        if (e.type() == Object && (e.fieldName()[0] == '$'
                                                     ? str::equals("query", e.fieldName()+1)
                                                     : str::equals("query", e.fieldName()))) {
                            // Extract the embedded query object.

                            if (cmdObj.hasField(Query::ReadPrefField.name())) {
                                // The command has a read preference setting. We don't want
                                // to lose this information so we copy this to a new field
                                // called $queryOptions.$readPreference
                                BSONObjBuilder finalCmdObjBuilder;
                                finalCmdObjBuilder.appendElements(e.embeddedObject());

                                BSONObjBuilder queryOptionsBuilder(
                                        finalCmdObjBuilder.subobjStart("$queryOptions"));
                                queryOptionsBuilder.append(cmdObj[Query::ReadPrefField.name()]);
                                queryOptionsBuilder.done();

                                cmdObj = finalCmdObjBuilder.obj();
                            }
                            else {
                                cmdObj = e.embeddedObject();
                            }
                        }
                    }

                    Command::runAgainstRegistered(q.ns, cmdObj, builder, q.queryOptions);
                    BSONObj x = builder.done();
                    replyToQuery(0, r.p(), r.m(), x);
                    return;
                }
                catch ( StaleConfigException& e ) {
                    if ( loops <= 0 )
                        throw e;

                    loops--;
                    log() << "retrying command: " << q.query << endl;

                    // For legacy reasons, ns may not actually be set in the exception :-(
                    string staleNS = e.getns();
                    if( staleNS.size() == 0 ) staleNS = q.ns;

                    ShardConnection::checkMyConnectionVersions( staleNS );
                    if( loops < 4 ) versionManager.forceRemoteCheckShardVersionCB( staleNS );
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
        }

        // Deprecated
        virtual void getMore( Request& r ) {
            // Don't use anymore, moved logic to strategy_shard, will remove in larger refactor
            verify( 0 );
        }

        // Deprecated
        virtual void writeOp( int op , Request& r ) {
            // Don't use anymore, requires single-step detection of chunk manager or primary
            verify( 0 );
        }

        bool handleSpecialNamespaces( Request& r , QueryMessage& q ) {
            const char * ns = strstr( r.getns() , ".$cmd.sys." );
            if ( ! ns )
                return false;
            ns += 10;

            BSONObjBuilder b;
            vector<Shard> shards;

            ClientBasic* client = ClientBasic::getCurrent();
            AuthorizationSession* authSession = client->getAuthorizationSession();
            if ( strcmp( ns , "inprog" ) == 0 ) {
                const bool isAuthorized = authSession->checkAuthorization(
                        AuthorizationManager::SERVER_RESOURCE_NAME, ActionType::inprog);
                audit::logInProgAuthzCheck(
                        client, q.query, isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
                uassert(ErrorCodes::Unauthorized, "not authorized to run inprog", isAuthorized);

                Shard::getAllShards( shards );

                BSONArrayBuilder arr( b.subarrayStart( "inprog" ) );

                for ( unsigned i=0; i<shards.size(); i++ ) {
                    Shard shard = shards[i];
                    ScopedDbConnection conn(shard.getConnString());
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
                const bool isAuthorized = authSession->checkAuthorization(
                        AuthorizationManager::SERVER_RESOURCE_NAME, ActionType::killop);
                audit::logKillOpAuthzCheck(
                        client,
                        q.query,
                        isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
                uassert(ErrorCodes::Unauthorized, "not authorized to run killop", isAuthorized);

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

                        ScopedDbConnection conn(s.getConnString());
                        conn->findOne( r.getns() , BSON( "op" << opid ) );
                        conn.done();
                    }
                }
            }
            else if ( strcmp( ns , "unlock" ) == 0 ) {
                b.append( "err" , "can't do unlock through mongos" );
            }
            else {
                warning() << "unknown sys command [" << ns << "]" << endl;
                return false;
            }

            BSONObj x = b.done();
            replyToQuery(0, r.p(), r.m(), x);
            return true;
        }
    };

    Strategy * SINGLE = new SingleStrategy();
}
