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
#include "../util/message.h"
#include "concurrency.h"
#include "pdfile.h"
#include "client.h"

namespace mongo {

//    void jniCallback(Message& m, Message& out);

    /* Note the limit here is rather arbitrary and is simply a standard. generally the code works
       with any object that fits in ram.

       Also note that the server has some basic checks to enforce this limit but those checks are not exhaustive
       for example need to check for size too big after
         update $push (append) operation
         various db.eval() type operations

       Note also we sometimes do work with objects slightly larger - an object in the replication local.oplog
       could be slightly larger.
    */
    const int MaxBSONObjectSize = 4 * 1024 * 1024;
    
    /**
     * class to hold path + dbname -> Database
     * might be able to optimizer further
     */
    class DatabaseHolder {
    public:
        typedef map<string,Database*> DBs;
        typedef map<string,DBs> Paths;

        DatabaseHolder() : _size(0){
        }

        bool isLoaded( const string& ns , const string& path ) const {
            dbMutex.assertAtLeastReadLocked();
            Paths::const_iterator x = _paths.find( path );
            if ( x == _paths.end() )
                return false;
            const DBs& m = x->second;
            
            string db = _todb( ns );

            DBs::const_iterator it = m.find(db);
            return it != m.end();
        }
        
        Database * get( const string& ns , const string& path ) const {
            dbMutex.assertAtLeastReadLocked();
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
        
        void put( const string& ns , const string& path , Database * db ){
            dbMutex.assertWriteLocked();
            DBs& m = _paths[path];
            Database*& d = m[_todb(ns)];
            if ( ! d )
                _size++;
            d = db;
        }
        
        Database* getOrCreate( const string& ns , const string& path , bool& justCreated ){
            dbMutex.assertWriteLocked();
            DBs& m = _paths[path];
            
            string dbname = _todb( ns );

            Database* & db = m[dbname];
            if ( db ){
                justCreated = false;
                return db;
            }
            
            log(1) << "Accessing: " << dbname << " for the first time" << endl;
            try {
                db = new Database( dbname.c_str() , justCreated , path );
            }
            catch ( ... ){
                m.erase( dbname );
                throw;
            }
            _size++;
            return db;
        }
        



        void erase( const string& ns , const string& path ){
            dbMutex.assertWriteLocked();
            DBs& m = _paths[path];
            _size -= (int)m.erase( _todb( ns ) );
        }

        /* force - force close even if something underway - use at shutdown */
        bool closeAll( const string& path , BSONObjBuilder& result, bool force );

        int size(){
            return _size;
        }
                
        void forEach(boost::function<void(Database *)> f) const {
            dbMutex.assertAtLeastReadLocked();
            for ( Paths::const_iterator i=_paths.begin(); i!=_paths.end(); i++ ){
                DBs m = i->second;
                for( DBs::const_iterator j=m.begin(); j!=m.end(); j++ ){
                    f(j->second);
                }
            }
        }         

        /**
         * gets all unique db names, ignoring paths
         */
        void getAllShortNames( set<string>& all ) const {
            dbMutex.assertAtLeastReadLocked();
            for ( Paths::const_iterator i=_paths.begin(); i!=_paths.end(); i++ ){
                DBs m = i->second;
                for( DBs::const_iterator j=m.begin(); j!=m.end(); j++ ){
                    all.insert( j->first );
                }
            }
        }

    private:
        
        string _todb( const string& ns ) const {
            string d = __todb( ns );
            uassert( 13280 , (string)"invalid db name: " + ns , Database::validDBName( d ) );            
            return d;
        }

        string __todb( const string& ns ) const {
            size_t i = ns.find( '.' );
            if ( i == string::npos ){
                uassert( 13074 , "db name can't be empty" , ns.size() );
                return ns;
            }
            uassert( 13075 , "db name can't be empty" , i > 0 );
            return ns.substr( 0 , i );
        }
        
        Paths _paths;
        int _size;
        
    };

    extern DatabaseHolder dbHolder;

    struct dbtemprelease {
        Client::Context * _context;
        int _locktype;
        
        dbtemprelease() {
            _context = cc().getContext();
            _locktype = dbMutex.getState();
            assert( _locktype );
            
            if ( _locktype > 0 ) {
				massert( 10298 , "can't temprelease nested write lock", _locktype == 1);
                if ( _context ) _context->unlocked();
                dbMutex.unlock();
			}
            else {
				massert( 10299 , "can't temprelease nested read lock", _locktype == -1);
                if ( _context ) _context->unlocked();
                dbMutex.unlock_shared();
			}

        }
        ~dbtemprelease() {
            if ( _locktype > 0 )
                dbMutex.lock();
            else
                dbMutex.lock_shared();
            
            if ( _context ) _context->relocked();
        }
    };
    

    /**
       only does a temp release if we're not nested and have a lock
     */
    struct dbtempreleasecond {
        dbtemprelease * real;
        int locktype;
        
        dbtempreleasecond(){
            real = 0;
            locktype = dbMutex.getState();
            if ( locktype == 1 || locktype == -1 )
                real = new dbtemprelease();
        }
        
        ~dbtempreleasecond(){
            if ( real ){
                delete real;
                real = 0;
            }
        }
        
        bool unlocked(){
            return real > 0;
        }
    };

} // namespace mongo

//#include "dbinfo.h"
#include "concurrency.h"
