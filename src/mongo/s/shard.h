// @file shard.h

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

#pragma once

#include "pch.h"

#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_rs.h"

namespace mongo {

    class ShardConnection;
    class ShardStatus;

    /*
     * A "shard" one partition of the overall database (and a replica set typically).
     */

    class Shard {
    public:
        Shard()
            : _name("") , _addr("") , _maxSize(0) , _isDraining( false ) {
        }

        Shard( const string& name , const string& addr, long long maxSize = 0 , bool isDraining = false )
            : _name(name) , _addr( addr ) , _maxSize( maxSize ) , _isDraining( isDraining ) {
            _setAddr( addr );
        }

        Shard( const string& ident ) {
            reset( ident );
        }

        Shard( const Shard& other )
            : _name( other._name ) , _addr( other._addr ) , _cs( other._cs ) , 
              _maxSize( other._maxSize ) , _isDraining( other._isDraining ) , _rs( other._rs ) {
        }

        Shard( const Shard* other )
            : _name( other->_name ) , _addr( other->_addr ), _cs( other->_cs ) , 
              _maxSize( other->_maxSize ) , _isDraining( other->_isDraining ) , _rs( other->_rs ) {
        }

        static Shard make( const string& ident ) {
            Shard s;
            s.reset( ident );
            return s;
        }

        /**
         * @param ident either name or address
         */
        void reset( const string& ident );

        void setAddress( const ConnectionString& cs );
        
        ConnectionString getAddress() const { return _cs; }

        string getName() const {
            verify( _name.size() );
            return _name;
        }

        string getConnString() const {
            verify( _addr.size() );
            return _addr;
        }

        long long getMaxSize() const {
            return _maxSize;
        }

        bool isDraining() const {
            return _isDraining;
        }

        string toString() const {
            return _name + ":" + _addr;
        }

        friend ostream& operator << (ostream& out, const Shard& s) {
            return (out << s.toString());
        }

        bool operator==( const Shard& s ) const {
            bool n = _name == s._name;
            bool a = _addr == s._addr;

            verify( n == a ); // names and address are 1 to 1
            return n;
        }

        bool operator!=( const Shard& s ) const {
            bool n = _name == s._name;
            bool a = _addr == s._addr;
            return ! ( n && a );
        }


        bool operator==( const string& s ) const {
            return _name == s || _addr == s;
        }

        bool operator!=( const string& s ) const {
            return _name != s && _addr != s;
        }

        bool operator<(const Shard& o) const {
            return _name < o._name;
        }

        bool ok() const { return _addr.size() > 0; }

        BSONObj runCommand( const string& db , const string& simple ) const {
            return runCommand( db , BSON( simple << 1 ) );
        }
        BSONObj runCommand( const string& db , const BSONObj& cmd ) const ;

        ShardStatus getStatus() const ;
        
        /**
         * mostly for replica set
         * retursn true if node is the shard 
         * of if the replica set contains node
         */
        bool containsNode( const string& node ) const;

        static void getAllShards( vector<Shard>& all );
        static void printShardInfo( ostream& out );
        static Shard lookupRSName( const string& name);
        
        /**
         * @parm current - shard where the chunk/database currently lives in
         * @return the currently emptiest shard, if best then current, or EMPTY
         */
        static Shard pick( const Shard& current = EMPTY );

        static void reloadShardInfo();

        static void removeShard( const string& name );

        static bool isAShardNode( const string& ident );

        static Shard EMPTY;
        
    private:
        
	void _rsInit();
        void _setAddr( const string& addr );
        
        string    _name;
        string    _addr;
        ConnectionString _cs;
        long long _maxSize;    // in MBytes, 0 is unlimited
        bool      _isDraining; // shard is currently being removed
        ReplicaSetMonitorPtr _rs;
    };

    class ShardStatus {
    public:

        ShardStatus( const Shard& shard , const BSONObj& obj );

        friend ostream& operator << (ostream& out, const ShardStatus& s) {
            out << s.toString();
            return out;
        }

        string toString() const {
            stringstream ss;
            ss << "shard: " << _shard << " mapped: " << _mapped << " writeLock: " << _writeLock;
            return ss.str();
        }

        bool operator<( const ShardStatus& other ) const {
            return _mapped < other._mapped;
        }

        Shard shard() const {
            return _shard;
        }

        long long mapped() const {
            return _mapped;
        }

        bool hasOpsQueued() const {
            return _hasOpsQueued;
        }

    private:
        Shard _shard;
        long long _mapped;
        bool _hasOpsQueued;  // true if 'writebacks' are pending
        double _writeLock;
    };

    class ChunkManager;
    typedef shared_ptr<const ChunkManager> ChunkManagerPtr;

    class ShardConnection : public AScopedConnection {
    public:
        ShardConnection( const Shard * s , const string& ns, ChunkManagerPtr manager = ChunkManagerPtr() );
        ShardConnection( const Shard& s , const string& ns, ChunkManagerPtr manager = ChunkManagerPtr() );
        ShardConnection( const string& addr , const string& ns, ChunkManagerPtr manager = ChunkManagerPtr() );

        ~ShardConnection();

        void done();
        void kill();

        DBClientBase& conn() {
            _finishInit();
            verify( _conn );
            return *_conn;
        }

        DBClientBase* operator->() {
            _finishInit();
            verify( _conn );
            return _conn;
        }

        DBClientBase* get() {
            _finishInit();
            verify( _conn );
            return _conn;
        }

        string getHost() const {
            return _addr;
        }

        string getNS() const {
            return _ns;
        }

        ChunkManagerPtr getManager() const {
            return _manager;
        }

        bool setVersion() {
            _finishInit();
            return _setVersion;
        }

        static void sync();

        void donotCheckVersion() {
            _setVersion = false;
            _finishedInit = true;
        }
        
        bool ok() const { return _conn > 0; }

        /**
           this just passes through excpet it checks for stale configs
         */
        bool runCommand( const string& db , const BSONObj& cmd , BSONObj& res );

        /** checks all of my thread local connections for the version of this ns */
        static void checkMyConnectionVersions( const string & ns );

    private:
        void _init();
        void _finishInit();

        bool _finishedInit;

        string _addr;
        string _ns;
        ChunkManagerPtr _manager;

        DBClientBase* _conn;
        bool _setVersion;
    };


    extern DBConnectionPool shardConnectionPool;

    class ShardingConnectionHook : public DBConnectionHook {
    public:

        ShardingConnectionHook( bool shardedConnections )
            : _shardedConnections( shardedConnections ) {
        }

        virtual void onCreate( DBClientBase * conn );
        virtual void onHandedOut( DBClientBase * conn );
        virtual void onDestroy( DBClientBase * conn );

        bool _shardedConnections;
    };
}
