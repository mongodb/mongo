// d_logic.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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


/**
   these are commands that live in mongod
   mostly around shard management and checking
 */

#include "stdafx.h"
#include <map>
#include <string>

#include "../db/commands.h"
#include "../db/jsobj.h"
#include "../db/dbmessage.h"

#include "../client/connpool.h"

#include "../util/queue.h"

using namespace std;

namespace mongo {
    
    typedef map<string,unsigned long long> NSVersions;
    
    NSVersions myVersions;
    boost::thread_specific_ptr<NSVersions> clientShardVersions;

    string shardConfigServer;

    boost::thread_specific_ptr<OID> clientServerIds;
    map< string , BlockingQueue<BSONObj>* > clientQueues;

    unsigned long long getVersion( BSONElement e , string& errmsg ){
        if ( e.eoo() ){
            errmsg = "no version";
            return 0;
        }
        
        if ( e.isNumber() )
            return (unsigned long long)e.number();
        
        if ( e.type() == Date || e.type() == Timestamp )
            return e.date();

        
        errmsg = "version is not a numberic type";
        return 0;
    }

    class MongodShardCommand : public Command {
    public:
        MongodShardCommand( const char * n ) : Command( n ){
        }
        virtual bool slaveOk(){
            return false;
        }
        virtual bool adminOnly() {
            return true;
        }
    };
    
    class WriteBackCommand : public MongodShardCommand {
    public:
        WriteBackCommand() : MongodShardCommand( "writebacklisten" ){}
        bool run(const char *cmdns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){

            BSONElement e = cmdObj.firstElement();
            if ( e.type() != jstOID ){
                errmsg = "need oid as first value";
                return 0;
            }
            
            const OID id = e.__oid();
            
            dbtemprelease unlock;
            
            if ( ! clientQueues[id.str()] )
                clientQueues[id.str()] = new BlockingQueue<BSONObj>();

            BSONObj z = clientQueues[id.str()]->blockingPop();
            log(1) << "WriteBackCommand got : " << z << endl;
            
            result.append( "data" , z );
            
            return true;
        }
    } writeBackCommand;

    // setShardVersion( ns )
    
    class SetShardVersion : public MongodShardCommand {
    public:
        SetShardVersion() : MongodShardCommand("setShardVersion"){}

        virtual void help( stringstream& help ) const {
            help << " example: { setShardVersion : 'alleyinsider.foo' , version : 1 , configdb : '' } ";
        }
        
        bool run(const char *cmdns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            
            string configdb = cmdObj["configdb"].valuestrsafe();
            { // configdb checking
                if ( configdb.size() == 0 ){
                    errmsg = "no configdb";
                    return false;
                }
                
                if ( shardConfigServer.size() == 0 ){
                    if ( ! cmdObj.getBoolField( "authoritative" ) ){
                        result.appendBool( "need_authoritative" , true );
                        errmsg = "first setShardVersion";
                        return false;
                    }
                    shardConfigServer = configdb;
                }
                else if ( shardConfigServer != configdb ){
                    errmsg = "specified a different configdb!";
                    return false;
                }
            }
            
            { // setting up ids
                if ( cmdObj["serverID"].type() != jstOID ){
                    // TODO: fix this
                    //errmsg = "need serverID to be an OID";
                    //return 0;
                }
                else {
                    OID clientId = cmdObj["serverID"].__oid();
                    if ( ! clientServerIds.get() ){
                        string s = clientId.str();
                        
                        OID * nid = new OID();
                        nid->init( s );
                        clientServerIds.reset( nid );
                        
                        if ( ! clientQueues[s] )
                            clientQueues[s] = new BlockingQueue<BSONObj>();
                    }
                    else if ( clientId != *clientServerIds.get() ){
                        errmsg = "server id has changed!";
                        return 0;
                    }
                }
            }
            
            unsigned long long version = getVersion( cmdObj["version"] , errmsg );
            if ( ! version )
                return false;

            NSVersions * versions = clientShardVersions.get();
            
            if ( ! versions ){
                log(1) << "entering shard mode for connection" << endl;
                versions = new NSVersions();
                clientShardVersions.reset( versions );
            }
            
            string ns = cmdObj["setShardVersion"].valuestrsafe();
            if ( ns.size() == 0 ){
                errmsg = "need to speciy fully namespace";
                return false;
            }

            unsigned long long& oldVersion = (*versions)[ns];
            if ( version < oldVersion ){
                errmsg = "you already have a newer version";
                result.appendTimestamp( "oldVersion" , oldVersion );
                result.appendTimestamp( "newVersion" , version );
                return false;
            }

            unsigned long long& myVersion = myVersions[ns];
            if ( version < myVersion ){
                errmsg = "going to older version for global";
                return false;
            }
            
            if ( myVersion == 0 && ! cmdObj.getBoolField( "authoritative" ) ){
                // need authoritative for first look
                result.appendBool( "need_authoritative" , true );
                result.append( "ns" , ns );
                errmsg = "first time for this ns";
                return false;
            }

            result.appendTimestamp( "oldVersion" , oldVersion );
            oldVersion = version;
            myVersion = version;

            result.append( "ok" , 1 );
            return 1;
        }
        
    } setShardVersion;
    
    class GetShardVersion : public MongodShardCommand {
    public:
        GetShardVersion() : MongodShardCommand("getShardVersion"){}

        virtual void help( stringstream& help ) const {
            help << " example: { getShardVersion : 'alleyinsider.foo'  } ";
        }
        
        bool run(const char *cmdns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            string ns = cmdObj["getShardVersion"].valuestrsafe();
            if ( ns.size() == 0 ){
                errmsg = "need to speciy fully namespace";
                return false;
            }
            
            result.append( "configServer" , shardConfigServer.c_str() );

            result.appendTimestamp( "global" , myVersions[ns] );
            if ( clientShardVersions.get() )
                result.appendTimestamp( "mine" , (*clientShardVersions.get())[ns] );
            else 
                result.appendTimestamp( "mine" , 0 );
            
            result.append( "ok" , 1 );
            return true;
        }
        
    } getShardVersion;
    
    class MoveShardStartCommand : public MongodShardCommand {
    public:
        MoveShardStartCommand() : MongodShardCommand( "movechunk.start" ){}
        virtual void help( stringstream& help ) const {
            help << "should not be calling this directly" << endl;
        }
        
        bool run(const char *cmdns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            // so i have to start clone, tell caller its ok to make change
            // at this point the caller locks me, and updates config db
            // then finish calls finish, and then deletes data when cursors are done
            
            string ns = cmdObj["movechunk.start"].valuestrsafe();
            string to = cmdObj["to"].valuestrsafe();
            string from = cmdObj["from"].valuestrsafe(); // my public address, a tad redundant, but safe
            BSONObj filter = cmdObj.getObjectField( "filter" );
            
            if ( ns.size() == 0 ){
                errmsg = "need to specify namespace in command";
                return false;
            }
            
            if ( to.size() == 0 ){
                errmsg = "need to specify server to move shard to";
                return false;
            }
            if ( from.size() == 0 ){
                errmsg = "need to specify server to move shard from (redundat i know)";
                return false;
            }
            
            if ( filter.isEmpty() ){
                errmsg = "need to specify a filter";
                return false;
            }
            
            log() << "got movechunk.start: " << cmdObj << endl;
            
            
            BSONObj res;
            bool ok;
            
            {
                dbtemprelease unlock;
                
                ScopedDbConnection conn( to );
                ok = conn->runCommand( "admin" , 
                                            BSON( "startCloneCollection" << ns <<
                                                  "from" << from <<
                                                  "query" << filter 
                                                  ) , 
                                            res );
                conn.done();
            }
            
            log() << "   movechunk.start res: " << res << endl;
            
            if ( ok ){
                result.append( res["finishToken"] );
            }

            return ok;
        }
        
    } moveShardStartCmd;

    class MoveShardFinishCommand : public MongodShardCommand {
    public:
        MoveShardFinishCommand() : MongodShardCommand( "movechunk.finish" ){}
        virtual void help( stringstream& help ) const {
            help << "should not be calling this directly" << endl;
        }
        
        bool run(const char *cmdns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            // see MoveShardStartCommand::run
            
            string ns = cmdObj["movechunk.finish"].valuestrsafe();
            if ( ns.size() == 0 ){
                errmsg = "need ns as cmd value";
                return false;
            }

            string to = cmdObj["to"].valuestrsafe();
            if ( to.size() == 0 ){
                errmsg = "need to specify server to move shard to";
                return false;
            }


            unsigned long long newVersion = getVersion( cmdObj["newVersion"] , errmsg );
            if ( newVersion == 0 ){
                errmsg = "have to specify new version number";
                return false;
            }
                                                        
            BSONObj finishToken = cmdObj.getObjectField( "finishToken" );
            if ( finishToken.isEmpty() ){
                errmsg = "need finishToken";
                return false;
            }
            
            if ( ns != finishToken["collection"].valuestrsafe() ){
                errmsg = "namespaced don't match";
                return false;
            }
            
            // now we're locked
            myVersions[ns] = newVersion;
            NSVersions * versions = clientShardVersions.get();
            if ( ! versions ){
                versions = new NSVersions();
                clientShardVersions.reset( versions );
            }
            (*versions)[ns] = newVersion;
            
            BSONObj res;
            bool ok;
            
            {
                dbtemprelease unlock;
                
                ScopedDbConnection conn( to );
                ok = conn->runCommand( "admin" , 
                                       BSON( "finishCloneCollection" << finishToken ) ,
                                       res );
                conn.done();
            }
            
            if ( ! ok ){
                // uh oh
                errmsg = "finishCloneCollection failed!";
                result << "finishError" << res;
                return false;
            }
            
            // wait until cursors are clean
            cerr << "WARNING: deleting data before ensuring no more cursors TODO" << endl;
            
            dbtemprelease unlock;

            DBDirectClient client;
            BSONObj removeFilter = finishToken.getObjectField( "query" );
            client.remove( ns , removeFilter );

            return true;
        }
        
    } moveShardFinishCmd;
    
    bool haveLocalShardingInfo( const string& ns ){
        if ( shardConfigServer.empty() )
            return false;
        

        unsigned long long version = myVersions[ns];
        if ( version == 0 )
            return false;
        
        NSVersions * versions = clientShardVersions.get();
        if ( ! versions )
            return false;
        
        return true;
    }

    /**
     * @ return true if not in sharded mode
                     or if version for this client is ok
     */
    bool shardVersionOk( const string& ns , string& errmsg ){
        if ( shardConfigServer.empty() ){
            return true;
        }
        
        unsigned long long version = myVersions[ns];
        if ( version == 0 ){
            return true;
        }
        
        NSVersions * versions = clientShardVersions.get();
        if ( ! versions ){
            // this means the client has nothing sharded
            // so this allows direct connections to do whatever they want
            // which i think is the correct behavior
            return true;
        }

        unsigned long long clientVersion = (*versions)[ns];

        if ( clientVersion == 0 ){
            errmsg = "client in sharded mode, but doesn't have version set for this collection";
            return false;
        }
        
        if ( clientVersion >= version ){
            return true;
        }

        errmsg = "your version is too old.  ";
        errmsg += " ns: " + ns;

        return false;
    }


    bool handlePossibleShardedMessage( Message &m, DbResponse &dbresponse ){
        int op = m.data->operation();
        if ( op < 2000 || op >= 3000 )
            return false;

        
        const char *ns = m.data->_data + 4;
        string errmsg;
        if ( shardVersionOk( ns , errmsg ) )
            return false;
        
        if ( doesOpGetAResponse( op ) ){
            BufBuilder b( 32768 );
            b.skip( sizeof( QueryResult ) );
            {
                BSONObj obj = BSON( "$err" << errmsg );
                b.append( obj.objdata() , obj.objsize() );
            }
            
            QueryResult *qr = (QueryResult*)b.buf();
            qr->resultFlags() = QueryResult::ResultFlag_ErrSet | QueryResult::ResultFlag_ShardConfigStale;
            qr->len = b.len();
            qr->setOperation( opReply );
            qr->cursorId = 0;
            qr->startingFrom = 0;
            qr->nReturned = 1;
            b.decouple();

            Message * resp = new Message();
            resp->setData( qr , true );
            
            dbresponse.response = resp;
            dbresponse.responseTo = m.data->id;
            return true;
        }
        
        OID * clientID = clientServerIds.get();
        massert( "write with bad shard config and no server id!" , clientID );
        
        log() << "got write with an old config - writing back" << endl;

        BSONObjBuilder b;
        b.appendBool( "writeBack" , true );
        b.append( "ns" , ns );
        b.appendBinData( "msg" , m.data->len , bdtCustom , (char*)(m.data) );
        log() << "writing back msg with len: " << m.data->len << " op: " << m.data->_operation << endl;
        clientQueues[clientID->str()]->push( b.obj() );

        return true;
    }
    

}
