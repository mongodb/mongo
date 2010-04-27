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

#include "../stdafx.h"
#include "../client/connpool.h"

namespace mongo {

    class ShardConnection;
    
    class Shard {
    public:
        Shard( const string& addr = "" )
            : _addr( addr ){
        }

        Shard( const Shard& other )
            : _addr( other._addr ){
        }

        Shard( const Shard* other )
            : _addr( other->_addr ){
        }
        
        string getConnString() const {
            assert( _addr.size() );
            return _addr;
        }

        string toString() const {
            return _addr;
        }

        operator string() const {
            return _addr;
        }
        
        bool operator==( const Shard& s ) const {
            return _addr == s._addr;
        }

        bool operator==( const string& s ) const {
            return _addr == s;
        }

        Shard& operator=( const string& s ){
            _addr = s;
            return *this;
        }

        bool ok() const {
            return _addr.size() > 0;
        }
        
        static Shard EMPTY;

    private:
        string _addr;
    };

    class ShardConnection {
    public:
        ShardConnection( const Shard * s )
            : _conn( s->getConnString() ){
        }

        ShardConnection( const Shard& s )
            : _conn( s.getConnString() ){
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
