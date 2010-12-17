/** @file dbclient_rs.h - connect to a Replica Set, from C++ */

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "../pch.h"
#include "dbclient.h"

namespace mongo {

    /** Use this class to connect to a replica set of servers.  The class will manage
       checking for which server in a replica set is master, and do failover automatically.
       
       This can also be used to connect to replica pairs since pairs are a subset of sets
       
	   On a failover situation, expect at least one operation to return an error (throw 
	   an exception) before the failover is complete.  Operations are not retried.
    */
    class DBClientReplicaSet : public DBClientBase {
        string _name;
        DBClientConnection * _currentMaster;
        vector<HostAndPort> _servers;
        vector<DBClientConnection*> _conns;        
        void _checkMaster();
        DBClientConnection * checkMaster();

    public:
        /** Call connect() after constructing. autoReconnect is always on for DBClientReplicaSet connections. */
        DBClientReplicaSet( const string& name , const vector<HostAndPort>& servers );
        virtual ~DBClientReplicaSet();

        /** Returns false if nomember of the set were reachable, or neither is
           master, although,
           when false returned, you can still try to use this connection object, it will
           try reconnects.
           */
        bool connect();

        /** Authorize.  Authorizes all nodes as needed
        */
        virtual bool auth(const string &dbname, const string &username, const string &pwd, string& errmsg, bool digestPassword = true );

        /** throws userassertion "no master found" */
        virtual
        auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                       const BSONObj *fieldsToReturn = 0, int queryOptions = 0 , int batchSize = 0 );

        /** throws userassertion "no master found" */
        virtual
        BSONObj findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn = 0, int queryOptions = 0);

        /** insert */
        virtual void insert( const string &ns , BSONObj obj ) {
            checkMaster()->insert(ns, obj);
        }

        /** insert multiple objects.  Note that single object insert is asynchronous, so this version 
            is only nominally faster and not worth a special effort to try to use.  */
        virtual void insert( const string &ns, const vector< BSONObj >& v ) {
            checkMaster()->insert(ns, v);
        }

        /** remove */
        virtual void remove( const string &ns , Query obj , bool justOne = 0 ) {
            checkMaster()->remove(ns, obj, justOne);
        }

        /** update */
        virtual void update( const string &ns , Query query , BSONObj obj , bool upsert = 0 , bool multi = 0 ) {
            return checkMaster()->update(ns, query, obj, upsert,multi);
        }
        
        virtual void killCursor( long long cursorID ){
            checkMaster()->killCursor( cursorID );
        }

        string toString();

        /* this is the callback from our underlying connections to notify us that we got a "not master" error.
         */
        void isntMaster() {
            _currentMaster = 0;
        }
        
        string getServerAddress() const;
        
        DBClientConnection& masterConn();
        DBClientConnection& slaveConn();

        virtual bool call( Message &toSend, Message &response, bool assertOk=true ) { return checkMaster()->call( toSend , response , assertOk ); }
        virtual void say( Message &toSend ) { checkMaster()->say( toSend ); }
        virtual bool callRead( Message& toSend , Message& response ){ return checkMaster()->callRead( toSend , response ); }

        virtual ConnectionString::ConnectionType type() const { return ConnectionString::SET; }  

        virtual bool isMember( const DBConnector * conn ) const;

        virtual void checkResponse( const char *data, int nReturned ) { checkMaster()->checkResponse( data , nReturned ); }
        
        virtual bool isFailed() const {
            return _currentMaster == 0 || _currentMaster->isFailed();
        }

    protected:                
        virtual void sayPiggyBack( Message &toSend ) { checkMaster()->say( toSend ); }
        
    };
    

}
