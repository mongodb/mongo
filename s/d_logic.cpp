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

#include "pch.h"
#include <map>
#include <string>

#include "../db/commands.h"
#include "../db/jsobj.h"
#include "../db/dbmessage.h"
#include "../db/query.h"

#include "../client/connpool.h"

#include "../util/queue.h"

#include "shard.h"
#include "d_logic.h"

using namespace std;

namespace mongo {
    
    NSVersionMap globalVersions;
    boost::thread_specific_ptr<NSVersionMap> clientShardVersions;

    string shardConfigServer;

    boost::thread_specific_ptr<OID> clientServerIds;

    unsigned long long extractVersion( BSONElement e , string& errmsg ){
        if ( e.eoo() ){
            errmsg = "no version";
            return 0;
        }
        
        if ( e.isNumber() )
            return (unsigned long long)e.number();
        
        if ( e.type() == Date || e.type() == Timestamp )
            return e._numberLong();

        
        errmsg = "version is not a numeric type";
        return 0;
    }

    class MongodShardCommand : public Command {
    public:
        MongodShardCommand( const char * n ) : Command( n ){
        }
        virtual bool slaveOk() const {
            return false;
        }
        virtual bool adminOnly() const {
            return true;
        }
    };
    


    // setShardVersion( ns )
    
    class SetShardVersion : public MongodShardCommand {
    public:
        SetShardVersion() : MongodShardCommand("setShardVersion"){}

        virtual void help( stringstream& help ) const {
            help << " example: { setShardVersion : 'alleyinsider.foo' , version : 1 , configdb : '' } ";
        }
        
        virtual LockType locktype() const { return WRITE; } // TODO: figure out how to make this not need to lock
 
        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            
            bool authoritative = cmdObj.getBoolField( "authoritative" );

            string configdb = cmdObj["configdb"].valuestrsafe();
            { // configdb checking
                if ( configdb.size() == 0 ){
                    errmsg = "no configdb";
                    return false;
                }
                
                if ( shardConfigServer.size() == 0 ){
                    if ( ! authoritative ){
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
                    }
                    else if ( clientId != *clientServerIds.get() ){
                        errmsg = "server id has changed!";
                        return 0;
                    }
                }
            }
            
            unsigned long long version = extractVersion( cmdObj["version"] , errmsg );
            if ( errmsg.size() ){
                return false;
            }

            NSVersionMap * versions = clientShardVersions.get();
            
            if ( ! versions ){
                log(1) << "entering shard mode for connection" << endl;
                versions = new NSVersionMap();
                clientShardVersions.reset( versions );
            }
            
            string ns = cmdObj["setShardVersion"].valuestrsafe();
            if ( ns.size() == 0 ){
                errmsg = "need to speciy fully namespace";
                return false;
            }

            unsigned long long& oldVersion = (*versions)[ns];
            unsigned long long& globalVersion = globalVersions[ns];
            
            if ( version == 0 && globalVersion == 0 ){
                // this connection is cleaning itself
                oldVersion = 0;
                return 1;
            }

            if ( version == 0 && globalVersion > 0 ){
                if ( ! authoritative ){
                    result.appendBool( "need_authoritative" , true );
                    result.appendTimestamp( "globalVersion" , globalVersion );
                    result.appendTimestamp( "oldVersion" , oldVersion );
                    errmsg = "dropping needs to be authoritative";
                    return 0;
                }
                log() << "wiping data for: " << ns << endl;
                result.appendTimestamp( "beforeDrop" , globalVersion );
                // only setting global version on purpose
                // need clients to re-find meta-data
                globalVersion = 0;
                oldVersion = 0;
                return 1;
            }

            if ( version < oldVersion ){
                errmsg = "you already have a newer version";
                result.appendTimestamp( "oldVersion" , oldVersion );
                result.appendTimestamp( "newVersion" , version );
                return false;
            }
            
            if ( version < globalVersion ){
                errmsg = "going to older version for global";
                return false;
            }
            
            if ( globalVersion == 0 && ! cmdObj.getBoolField( "authoritative" ) ){
                // need authoritative for first look
                result.appendBool( "need_authoritative" , true );
                result.append( "ns" , ns );
                errmsg = "first time for this ns";
                return false;
            }

            result.appendTimestamp( "oldVersion" , oldVersion );
            oldVersion = version;
            globalVersion = version;

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
        
        virtual LockType locktype() const { return WRITE; } // TODO: figure out how to make this not need to lock

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            string ns = cmdObj["getShardVersion"].valuestrsafe();
            if ( ns.size() == 0 ){
                errmsg = "need to speciy fully namespace";
                return false;
            }
            
            result.append( "configServer" , shardConfigServer.c_str() );

            result.appendTimestamp( "global" , globalVersions[ns] );
            if ( clientShardVersions.get() )
                result.appendTimestamp( "mine" , (*clientShardVersions.get())[ns] );
            else 
                result.appendTimestamp( "mine" , 0 );
            
            return true;
        }
        
    } getShardVersion;
    

    
    bool haveLocalShardingInfo( const string& ns ){
        if ( shardConfigServer.empty() )
            return false;
        

        unsigned long long version = globalVersions[ns];
        if ( version == 0 )
            return false;
        
        NSVersionMap * versions = clientShardVersions.get();
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

        NSVersionMap::iterator i = globalVersions.find( ns );
        if ( i == globalVersions.end() )
            return true;
        
        NSVersionMap * versions = clientShardVersions.get();
        if ( ! versions ){
            // this means the client has nothing sharded
            // so this allows direct connections to do whatever they want
            // which i think is the correct behavior
            return true;
        }

        unsigned long long clientVersion = (*versions)[ns];
        unsigned long long version = i->second;
                
        if ( version == 0 && clientVersion > 0 ){
            stringstream ss;
            ss << "version: " << version << " clientVersion: " << clientVersion;
            errmsg = ss.str();
            return false;
        }
        
        if ( clientVersion >= version )
            return true;
        

        if ( clientVersion == 0 ){
            errmsg = "client in sharded mode, but doesn't have version set for this collection";
            return false;
        }

        errmsg = (string)"your version is too old  ns: " + ns;
        return false;
    }


    bool handlePossibleShardedMessage( Message &m, DbResponse &dbresponse ){

        if ( shardConfigServer.empty() ){
            return false;
        }

        int op = m.operation();
        if ( op < 2000 
             || op >= 3000 
             || op == dbGetMore  // cursors are weird
             )
            return false;

        
        const char *ns = m.singleData()->_data + 4;
        string errmsg;
        if ( shardVersionOk( ns , errmsg ) ){
            return false;
        }

        log() << "shardVersionOk failed  ns:" << ns << " " << errmsg << endl;
        
        if ( doesOpGetAResponse( op ) ){
            BufBuilder b( 32768 );
            b.skip( sizeof( QueryResult ) );
            {
                BSONObj obj = BSON( "$err" << errmsg );
                b.append( obj.objdata() , obj.objsize() );
            }
            
            QueryResult *qr = (QueryResult*)b.buf();
            qr->_resultFlags() = QueryResult::ResultFlag_ErrSet | QueryResult::ResultFlag_ShardConfigStale;
            qr->len = b.len();
            qr->setOperation( opReply );
            qr->cursorId = 0;
            qr->startingFrom = 0;
            qr->nReturned = 1;
            b.decouple();

            Message * resp = new Message();
            resp->setData( qr , true );
            
            dbresponse.response = resp;
            dbresponse.responseTo = m.header()->id;
            return true;
        }
        
        OID * clientID = clientServerIds.get();
        massert( 10422 ,  "write with bad shard config and no server id!" , clientID );
        
        log() << "got write with an old config - writing back" << endl;

        BSONObjBuilder b;
        b.appendBool( "writeBack" , true );
        b.append( "ns" , ns );
        b.appendBinData( "msg" , m.header()->len , bdtCustom , (char*)(m.singleData()) );
        log() << "writing back msg with len: " << m.header()->len << " op: " << m.operation() << endl;
        queueWriteBack( clientID->str() , b.obj() );

        return true;
    }

}
