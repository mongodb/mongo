// parallel.h

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

/**
   tools for working in parallel/sharded/clustered environment
 */

#include "../pch.h"
#include "dbclient.h"
#include "redef_macros.h"
#include "../db/dbmessage.h"
#include "../db/matcher.h"
#include "../util/concurrency/mvar.h"

namespace mongo {

    /**
     * holder for a server address and a query to run
     */
    class ServerAndQuery {
    public:
        ServerAndQuery( const string& server , BSONObj extra = BSONObj() , BSONObj orderObject = BSONObj() ) : 
            _server( server ) , _extra( extra.getOwned() ) , _orderObject( orderObject.getOwned() ){
        }

        bool operator<( const ServerAndQuery& other ) const{
            if ( ! _orderObject.isEmpty() )
                return _orderObject.woCompare( other._orderObject ) < 0;
            
            if ( _server < other._server )
                return true;
            if ( other._server > _server )
                return false;
            return _extra.woCompare( other._extra ) < 0;
        }

        string toString() const {
            StringBuilder ss;
            ss << "server:" << _server << " _extra:" << _extra.toString() << " _orderObject:" << _orderObject.toString();
            return ss.str();
        }

        operator string() const {
            return toString();
        }

        string _server;
        BSONObj _extra;
        BSONObj _orderObject;
    };

    /**
     * this is a cursor that works over a set of servers
     * can be used in serial/paralellel as controlled by sub classes
     */
    class ClusteredCursor {
    public:
        ClusteredCursor( QueryMessage& q );
        ClusteredCursor( const string& ns , const BSONObj& q , int options=0 , const BSONObj& fields=BSONObj() );
        virtual ~ClusteredCursor();
        
        /** call before using */
        void init();
        
        virtual bool more() = 0;
        virtual BSONObj next() = 0;
        
        static BSONObj concatQuery( const BSONObj& query , const BSONObj& extraFilter );
        
        virtual string type() const = 0;

        virtual BSONObj explain();

    protected:
        
        virtual void _init() = 0;

        auto_ptr<DBClientCursor> query( const string& server , int num = 0 , BSONObj extraFilter = BSONObj() , int skipLeft = 0 );
        BSONObj explain( const string& server , BSONObj extraFilter = BSONObj() );
        
        static BSONObj _concatFilter( const BSONObj& filter , const BSONObj& extraFilter );
        
        virtual void _explain( map< string,list<BSONObj> >& out ) = 0;

        string _ns;
        BSONObj _query;
        int _options;
        BSONObj _fields;
        int _batchSize;

        bool _didInit;

        bool _done;
    };


    class FilteringClientCursor {
    public:
        FilteringClientCursor( const BSONObj filter = BSONObj() );
        FilteringClientCursor( auto_ptr<DBClientCursor> cursor , const BSONObj filter = BSONObj() );
        ~FilteringClientCursor();
        
        void reset( auto_ptr<DBClientCursor> cursor );
        
        bool more();
        BSONObj next();
        
        BSONObj peek();
    private:
        void _advance();
        
        Matcher _matcher;
        auto_ptr<DBClientCursor> _cursor;
        
        BSONObj _next;
        bool _done;
    };


    class Servers {
    public:
        Servers(){
        }
        
        void add( const ServerAndQuery& s ){
            add( s._server , s._extra );
        }
        
        void add( const string& server , const BSONObj& filter ){
            vector<BSONObj>& mine = _filters[server];
            mine.push_back( filter.getOwned() );
        }
        
        // TOOO: pick a less horrible name
        class View {
            View( const Servers* s ){
                for ( map<string, vector<BSONObj> >::const_iterator i=s->_filters.begin(); i!=s->_filters.end(); ++i ){
                    _servers.push_back( i->first );
                    _filters.push_back( i->second );
                }
            }
        public:
            int size() const {
                return _servers.size();
            }

            string getServer( int n ) const {
                return _servers[n];
            }

            vector<BSONObj> getFilter( int n ) const {
                return _filters[ n ];
            }
            
        private:
            vector<string> _servers;
            vector< vector<BSONObj> > _filters;

            friend class Servers;
        };

        View view() const {
            return View( this );
        }
        

    private:
        map<string, vector<BSONObj> > _filters;

        friend class View;
    };


    /**
     * runs a query in serial across any number of servers
     * returns all results from 1 server, then the next, etc...
     */
    class SerialServerClusteredCursor : public ClusteredCursor {
    public:
        SerialServerClusteredCursor( const set<ServerAndQuery>& servers , QueryMessage& q , int sortOrder=0);
        virtual bool more();
        virtual BSONObj next();
        virtual string type() const { return "SerialServer"; }

    protected:
        virtual void _explain( map< string,list<BSONObj> >& out );

        void _init(){}

        vector<ServerAndQuery> _servers;
        unsigned _serverIndex;
        
        FilteringClientCursor _current;
        
        int _needToSkip;
    };


    /**
     * runs a query in parellel across N servers
     * sots
     */        
    class ParallelSortClusteredCursor : public ClusteredCursor {
    public:
        ParallelSortClusteredCursor( const set<ServerAndQuery>& servers , QueryMessage& q , const BSONObj& sortKey );
        ParallelSortClusteredCursor( const set<ServerAndQuery>& servers , const string& ns , 
                                     const Query& q , int options=0, const BSONObj& fields=BSONObj() );
        virtual ~ParallelSortClusteredCursor();
        virtual bool more();
        virtual BSONObj next();
        virtual string type() const { return "ParallelSort"; }
    protected:
        void _finishCons();
        void _init();

        virtual void _explain( map< string,list<BSONObj> >& out );

        int _numServers;
        set<ServerAndQuery> _servers;
        BSONObj _sortKey;
        
        FilteringClientCursor * _cursors;
        int _needToSkip;
    };

    /**
     * tools for doing asynchronous operations
     * right now uses underlying sync network ops and uses another thread
     * should be changed to use non-blocking io
     */
    class Future {
    public:
        class CommandResult {
        public:
            
            string getServer() const { return _server; }

            bool isDone() const { return _done; }
            
            bool ok() const {
                assert( _done );
                return _ok;
            }

            BSONObj result() const {
                assert( _done );
                return _res;
            }

            /**
               blocks until command is done
               returns ok()
             */
            bool join();
            
        private:
            
            CommandResult( const string& server , const string& db , const BSONObj& cmd );
            
            string _server;
            string _db;
            BSONObj _cmd;

            scoped_ptr<boost::thread> _thr;
            
            BSONObj _res;
            bool _ok;
            bool _done;
            
            friend class Future;
        };
        
        static void commandThread(shared_ptr<CommandResult> res);
        
        static shared_ptr<CommandResult> spawnCommand( const string& server , const string& db , const BSONObj& cmd );
    };

    
}

#include "undef_macros.h"
