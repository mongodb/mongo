// syncclusterconnection.h
/*
 *    Copyright 2010 10gen Inc.
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


#include "../stdafx.h"
#include "dbclient.h"

namespace mongo {

    /**
     * this is a connection to a cluster of servers that operate as one
     * for super high durability
     */
    class SyncClusterConnection : public DBClientBase {
    public:
        /**
         * @param commaSeperated should be 3 hosts comma seperated
         */
        SyncClusterConnection( string commaSeperated );
        SyncClusterConnection( string a , string b , string c );
        ~SyncClusterConnection();
        
        
        /**
         * @return true if all servers are up and ready for writes
         */
        bool prepare( string& errmsg );

        /**
         * runs fsync on all servers
         */
        bool fsync( string& errmsg );

        // --- from DBClientInterface

        virtual BSONObj findOne(const string &ns, Query query, const BSONObj *fieldsToReturn, int queryOptions);

        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn, int nToSkip,
                                               const BSONObj *fieldsToReturn, int queryOptions, int batchSize );

        virtual auto_ptr<DBClientCursor> getMore( const string &ns, long long cursorId, int nToReturn, int options );
        
        virtual void insert( const string &ns, BSONObj obj );
        
        virtual void insert( const string &ns, const vector< BSONObj >& v );

        virtual void remove( const string &ns , Query query, bool justOne );

        virtual void update( const string &ns , Query query , BSONObj obj , bool upsert , bool multi );

        virtual string toString(){
            return _toString();
        }

        virtual bool call( Message &toSend, Message &response, bool assertOk );
        virtual void say( Message &toSend );
        virtual void sayPiggyBack( Message &toSend );
        
        virtual string getServerAddress() const { return _address; }

        virtual bool isFailed() const { 
            return false; 
        }

    private:
        
        SyncClusterConnection( SyncClusterConnection& prev );

        string _toString() const;
        
        bool _commandOnActive(const string &dbname, const BSONObj& cmd, BSONObj &info, int options=0);

        auto_ptr<DBClientCursor> _queryOnActive(const string &ns, Query query, int nToReturn, int nToSkip,
                                                const BSONObj *fieldsToReturn, int queryOptions, int batchSize );
        
        int _lockType( const string& name );

        void _checkLast();
        
        void _connect( string host );

        string _address;
        vector<DBClientConnection*> _conns;
        
        map<string,int> _lockTypes;
        mongo::mutex _mutex;
    };
    

};
