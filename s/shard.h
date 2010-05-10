// shard.h

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

#include "../pch.h"
#include "../client/connpool.h"

namespace mongo {

    class ShardConnection;
    class ShardStatus;

    class Shard {
    public:
        Shard()
            : _name(""),_addr(""){
        }
        Shard( const string& name , const string& addr )
            : _name(name), _addr( addr ){
        }

        Shard( const string& ident ){
            reset( ident );
        }

        Shard( const Shard& other )
            : _name( other._name ) , _addr( other._addr ){
        }

        Shard( const Shard* other )
            : _name( other->_name ) ,_addr( other->_addr ){
        }
        
        static Shard make( const string& ident ){
            Shard s;
            s.reset( ident );
            return s;
        }
        
        /**
         * @param ident either name or address
         */
        void reset( const string& ident );
        
        void setAddress( const string& addr , bool authoritative = false );

        string getName() const {
            assert( _name.size() );
            return _name;
        }
        
        string getConnString() const {
            assert( _addr.size() );
            return _addr;
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
            
            assert( n == a ); // names and address are 1 to 1
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
        
        bool ok() const {
            return _addr.size() > 0 && _addr.size() > 0;
        }
        
        BSONObj runCommand( const string& db , const string& simple ) const {
            return runCommand( db , BSON( simple << 1 ) );
        }
        BSONObj runCommand( const string& db , const BSONObj& cmd ) const ;
        
        ShardStatus getStatus() const ;
        
        static void getAllShards( vector<Shard>& all );
        
        /**
         * picks a Shard for more load
         */
        static Shard pick();
        
        static void reloadShardInfo();

        static Shard EMPTY;

    private:
        string _name;
        string _addr;
    };

    class ShardStatus {
    public:
        
        ShardStatus( const Shard& shard , const BSONObj& obj );

        friend ostream& operator << (ostream& out, const ShardStatus& s) {
            out << (string)s;
            return out;
        }

        operator string() const {
            stringstream ss;
            ss << "shard: " << _shard << " mapped: " << _mapped << " writeLock: " << _writeLock; 
            return ss.str();
        }

        bool operator<( const ShardStatus& other ) const{
            return _mapped < other._mapped;
        }
        
        Shard shard() const {
            return _shard;
        }

    private:
        Shard _shard;
        long long _mapped;
        double _writeLock;
    };

    class ShardConnection {
    public:
        ShardConnection( const Shard * s )
            : _conn( s->getConnString() ){
        }

        ShardConnection( const Shard& s )
            : _conn( s.getConnString() ){
        }
        
        ShardConnection( const string& addr )
            : _conn( addr ){
        }

        void done(){
            _conn.done();
        }
        
        void kill(){
            _conn.kill();
        }
        
        DBClientBase& conn(){
            return _conn.conn();
        }
        
        DBClientBase* operator->(){
            return _conn.get();
        }
        
    private:
        ScopedDbConnection _conn;
    };
}
