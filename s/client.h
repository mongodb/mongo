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
        
        string getRemote() const { return _remote; }

        void addShard( const string& shard );
        set<string> * getPrev() const { return _prev; };
        
        void newRequest( AbstractMessagingPort* p = 0 );
        void disconnect();
        
        static ClientInfo * get( int clientId = 0 , bool create = true );
        static void disconnect( int clientId );
        
        const set<string>& sinceLastGetError() const { return _sinceLastGetError; }
        void clearSinceLastGetError(){ 
            _sinceLastGetError.clear(); 
        }

    private:
        int _id;
        string _remote;

        set<string> _a;
        set<string> _b;
        set<string> * _cur;
        set<string> * _prev;
        int _lastAccess;
        
        set<string> _sinceLastGetError;

        static mongo::mutex _clientsLock;
        static Cache& _clients;
        static boost::thread_specific_ptr<ClientInfo> _tlInfo;
    };


}
