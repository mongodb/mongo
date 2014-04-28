// @file syncclusterconnection.h

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

#pragma once


#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/export_macros.h"

namespace mongo {

    /**
     * This is a connection to a cluster of servers that operate as one
     * for super high durability.
     *
     * Write operations are two-phase.  First, all nodes are asked to fsync. If successful
     * everywhere, the write is sent everywhere and then followed by an fsync.  There is no
     * rollback if a problem occurs during the second phase.  Naturally, with all these fsyncs,
     * these operations will be quite slow -- use sparingly.
     *
     * Read operations are sent to a single random node.
     *
     * The class checks if a command is read or write style, and sends to a single
     * node if a read lock command and to all in two phases with a write style command.
     */
    class MONGO_CLIENT_API SyncClusterConnection : public DBClientBase {
    public:

        using DBClientBase::query;
        using DBClientBase::update;
        using DBClientBase::remove;

        class QueryHandler;

        /**
         * @param commaSeparated should be 3 hosts comma separated
         */
        SyncClusterConnection( const list<HostAndPort> &, double socketTimeout = 0);
        SyncClusterConnection( string commaSeparated, double socketTimeout = 0);
        SyncClusterConnection( const std::string& a,
                               const std::string& b,
                               const std::string& c,
                               double socketTimeout = 0 );
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

        virtual BSONObj findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn, int queryOptions);

        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn, int nToSkip,
                                               const BSONObj *fieldsToReturn, int queryOptions, int batchSize );

        virtual auto_ptr<DBClientCursor> getMore( const string &ns, long long cursorId, int nToReturn, int options );

        virtual void insert( const string &ns, BSONObj obj, int flags=0);

        virtual void insert( const string &ns, const vector< BSONObj >& v, int flags=0);

        virtual void remove( const string &ns , Query query, int flags );

        virtual void update( const string &ns , Query query , BSONObj obj , int flags );

        virtual bool call( Message &toSend, Message &response, bool assertOk , string * actualServer );
        virtual void say( Message &toSend, bool isRetry = false , string * actualServer = 0 );
        virtual void sayPiggyBack( Message &toSend );

        virtual void killCursor( long long cursorID );

        virtual string getServerAddress() const { return _address; }
        virtual bool isFailed() const { return false; }
        virtual bool isStillConnected();
        virtual string toString() const { return _toString(); }

        virtual BSONObj getLastErrorDetailed(const std::string& db,
                                             bool fsync=false,
                                             bool j=false,
                                             int w=0,
                                             int wtimeout=0);
        virtual BSONObj getLastErrorDetailed(bool fsync=false, bool j=false, int w=0, int wtimeout=0);

        virtual bool callRead( Message& toSend , Message& response );

        virtual ConnectionString::ConnectionType type() const { return ConnectionString::SYNC; }

        void setAllSoTimeouts( double socketTimeout );
        double getSoTimeout() const { return _socketTimeout; }


        virtual bool lazySupported() const { return false; }

        virtual void setRunCommandHook(DBClientWithCommands::RunCommandHookFunc func);
        virtual void setPostRunCommandHook(DBClientWithCommands::PostRunCommandHookFunc func);

        /**
         * Allow custom query processing through an external (e.g. mongos-only) service.
         *
         * Takes ownership of attached handler.
         */
        void attachQueryHandler( QueryHandler* handler );

    protected:
        virtual void _auth(const BSONObj& params);

    private:
        SyncClusterConnection( SyncClusterConnection& prev, double socketTimeout = 0 );
        string _toString() const;
        bool _commandOnActive(const string &dbname, const BSONObj& cmd, BSONObj &info, int options=0);
        auto_ptr<DBClientCursor> _queryOnActive(const string &ns, Query query, int nToReturn, int nToSkip,
                                                const BSONObj *fieldsToReturn, int queryOptions, int batchSize );
        int _lockType( const string& name );
        void _checkLast();
        void _connect( const std::string& host );

        string _address;
        vector<string> _connAddresses;
        vector<DBClientConnection*> _conns;

        vector<BSONObj> _lastErrors;

        // Optionally attached by user
        scoped_ptr<QueryHandler> _customQueryHandler;

        mongo::mutex _mutex;
        map<string,int> _lockTypes;
        // End mutex

        double _socketTimeout;
    };

    /**
     * Interface for custom query processing for the SCC.
     * Allows plugging different host query behaviors for different types of queries.
     */
    class SyncClusterConnection::QueryHandler {
    public:

        virtual ~QueryHandler() {};

        /**
         * Returns true if the query can be processed using this handler.
         */
        virtual bool canHandleQuery( const string& ns, Query query ) = 0;

        /**
         * Returns a cursor on one of the hosts with the desired results for the query.
         * May throw or return an empty auto_ptr on failure.
         */
        virtual auto_ptr<DBClientCursor> handleQuery( const vector<string>& hosts,
                                                      const string &ns,
                                                      Query query,
                                                      int nToReturn,
                                                      int nToSkip,
                                                      const BSONObj *fieldsToReturn,
                                                      int queryOptions,
                                                      int batchSize ) = 0;
    };

    class MONGO_CLIENT_API UpdateNotTheSame : public UserException {
    public:
        UpdateNotTheSame( int code , const string& msg , const vector<string>& addrs , const vector<BSONObj>& lastErrors )
            : UserException( code , msg ) , _addrs( addrs ) , _lastErrors( lastErrors ) {
            verify( _addrs.size() == _lastErrors.size() );
        }

        virtual ~UpdateNotTheSame() throw() {
        }

        unsigned size() const {
            return _addrs.size();
        }

        pair<string,BSONObj> operator[](unsigned i) const {
            return make_pair( _addrs[i] , _lastErrors[i] );
        }

    private:

        vector<string> _addrs;
        vector<BSONObj> _lastErrors;
    };

};
