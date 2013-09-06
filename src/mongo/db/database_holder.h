// @file databaseholder.h

/**
*    Copyright (C) 2012 10gen Inc.
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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/db/database.h"
#include "mongo/db/namespace_string.h"

namespace mongo { 

    /**
     * path + dbname -> Database
     */
    class DatabaseHolder {
        typedef map<string,Database*> DBs;
        typedef map<string,DBs> Paths;
        // todo: we want something faster than this if called a lot:
        mutable SimpleMutex _m;
        Paths _paths;
        int _size;
    public:
        DatabaseHolder() : _m("dbholder"),_size(0) { }

        bool __isLoaded( const string& ns , const string& path ) const {
            SimpleMutex::scoped_lock lk(_m);
            Paths::const_iterator x = _paths.find( path );
            if ( x == _paths.end() )
                return false;
            const DBs& m = x->second;

            string db = _todb( ns );

            DBs::const_iterator it = m.find(db);
            return it != m.end();
        }
        // must be write locked as otherwise isLoaded could go false->true on you 
        // in the background and you might not expect that.
        bool _isLoaded( const string& ns , const string& path ) const {
            Lock::assertWriteLocked(ns);
            return __isLoaded(ns,path);
        }

        Database * get( const string& ns , const string& path ) const {
            SimpleMutex::scoped_lock lk(_m);
            Lock::assertAtLeastReadLocked(ns);
            Paths::const_iterator x = _paths.find( path );
            if ( x == _paths.end() )
                return 0;
            const DBs& m = x->second;
            string db = _todb( ns );
            DBs::const_iterator it = m.find(db);
            if ( it != m.end() )
                return it->second;
            return 0;
        }

        Database* getOrCreate( const string& ns , const string& path , bool& justCreated );

        void erase( const string& ns , const string& path ) {
            SimpleMutex::scoped_lock lk(_m);
            verify( Lock::isW() );
            DBs& m = _paths[path];
            _size -= (int)m.erase( _todb( ns ) );
        }

        /** @param force - force close even if something underway - use at shutdown */
        bool closeAll( const string& path , BSONObjBuilder& result, bool force );

        // "info" as this is informational only could change on you if you are not write locked
        int sizeInfo() const { return _size; }

        /**
         * gets all unique db names, ignoring paths
         * need some lock
         */
        void getAllShortNames( set<string>& all ) const {
            SimpleMutex::scoped_lock lk(_m);
            for ( Paths::const_iterator i=_paths.begin(); i!=_paths.end(); i++ ) {
                DBs m = i->second;
                for( DBs::const_iterator j=m.begin(); j!=m.end(); j++ ) {
                    all.insert( j->first );
                }
            }
        }

    private:
        static string _todb( const string& ns ) {
            string d = __todb( ns );
            uassert( 13280 , (string)"invalid db name: " + ns , NamespaceString::validDBName( d ) );
            return d;
        }
        static string __todb( const string& ns ) {
            size_t i = ns.find( '.' );
            if ( i == string::npos ) {
                uassert( 13074 , "db name can't be empty" , ns.size() );
                return ns;
            }
            uassert( 13075 , "db name can't be empty" , i > 0 );
            return ns.substr( 0 , i );
        }
    };

    DatabaseHolder& dbHolderUnchecked();
    inline const DatabaseHolder& dbHolder() { 
        dassert( Lock::isLocked() );
        return dbHolderUnchecked();
    }
    inline DatabaseHolder& dbHolderW() { 
        dassert( Lock::isW() );
        return dbHolderUnchecked();
    }

}
