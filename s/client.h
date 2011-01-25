// client.h

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

#include "../pch.h"

namespace mongo {

    /**
     * holds information about a client connected to a mongos
     * 1 per client socket
     * currently implemented with a thread local
     */
    class ClientInfo {

        typedef map<int,ClientInfo*> Cache;

    public:
        ClientInfo( int clientId );
        ~ClientInfo();

        /** new request from client, adjusts internal state */
        void newRequest( AbstractMessagingPort* p = 0 );

        /** client disconnected */
        void disconnect();

        /**
         * @return remote socket address of the client
         */
        string getRemote() const { return _remote; }

        /**
         * notes that this client use this shard
         * keeps track of all shards accessed this request
         */
        void addShard( const string& shard );

        /**
         * gets shards used on the previous request
         */
        set<string> * getPrev() const { return _prev; };

        /**
         * gets all shards we've accessed since the last time we called clearSinceLastGetError
         */
        const set<string>& sinceLastGetError() const { return _sinceLastGetError; }

        /**
         * clears list of shards we've talked to
         */
        void clearSinceLastGetError() { _sinceLastGetError.clear(); }

        /**
         * calls getLastError
         * resets shards since get last error
         * @return if the command was ok or if there was an error
         */
        bool getLastError( const BSONObj& options , BSONObjBuilder& result , bool fromWriteBackListener = false );

        /** @return if its ok to auto split from this client */
        bool autoSplitOk() const { return _autoSplitOk; }
        
        void noAutoSplit() { _autoSplitOk = false; }

        static ClientInfo * get( int clientId = 0 , bool create = true );
        static void disconnect( int clientId );

    private:

        struct WBInfo {
            WBInfo( ConnectionId c , OID o ) : connectionId( c ) , id( o ) {}
            ConnectionId connectionId;
            OID id;
        };

        // for getLastError
        void _addWriteBack( vector<WBInfo>& all , const BSONObj& o );
        vector<BSONObj> _handleWriteBacks( vector<WBInfo>& all , bool fromWriteBackListener );


        int _id; // unique client id
        string _remote; // server:port of remote socket end

        // we use _a and _b to store shards we've talked to on the current request and the previous
        // we use 2 so we can flip for getLastError type operations

        set<string> _a; // actual set for _cur or _prev
        set<string> _b; //   "

        set<string> * _cur; // pointer to _a or _b depending on state
        set<string> * _prev; //  ""


        set<string> _sinceLastGetError; // all shards accessed since last getLastError

        int _lastAccess;
        bool _autoSplitOk; 

        static mongo::mutex _clientsLock;
        static Cache& _clients;
        static boost::thread_specific_ptr<ClientInfo> _tlInfo;
    };


}
