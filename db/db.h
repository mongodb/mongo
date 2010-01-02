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
#include "../util/message.h"
#include "../util/top.h"
#include "boost/version.hpp"
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
        DatabaseHolder() : _size(0){
        }

        Database * get( const string& ns , const string& path ){
            dbMutex.assertAtLeastReadLocked();
            map<string,Database*>& m = _paths[path];
            
            string db = _todb( ns );

            map<string,Database*>::iterator it = m.find(db);
            if ( it != m.end() ) 
                return it->second;
            return 0;
        }
        
        void put( const string& ns , const string& path , Database * db ){
            dbMutex.assertWriteLocked();
            map<string,Database*>& m = _paths[path];
            Database*& d = m[_todb(ns)];
            if ( ! d )
                _size++;
            d = db;
        }
        
        void erase( const string& ns , const string& path ){
            dbMutex.assertWriteLocked();
            map<string,Database*>& m = _paths[path];
            _size -= m.erase( _todb( ns ) );
        }

        bool closeAll( const string& path , BSONObjBuilder& result );

        int size(){
            return _size;
        }
        
        /**
         * gets all unique db names, ignoring paths
         */
        void getAllShortNames( set<string>& all ) const{
            dbMutex.assertAtLeastReadLocked();
            for ( map<string, map<string,Database*> >::const_iterator i=_paths.begin(); i!=_paths.end(); i++ ){
                map<string,Database*> m = i->second;
                for( map<string,Database*>::const_iterator j=m.begin(); j!=m.end(); j++ ){
                    all.insert( j->first );
                }
            }
        }
        
    private:
        
        string _todb( const string& ns ){
            size_t i = ns.find( '.' );
            if ( i == string::npos )
                return ns;
            return ns.substr( 0 , i );
        }
        
        map<string, map<string,Database*> > _paths;
        int _size;
        
    };

    extern DatabaseHolder dbHolder;

    inline void resetClient(const char *ns, const string& path=dbpath) {
        dbMutex.assertAtLeastReadLocked();
        Database * d = dbHolder.get( ns , path );
        if ( d ){
            cc().setns(ns, d);
            return;
        }
        assert(false);
    }

    /* returns true if the database ("database") did not exist, and it was created on this call 
       path - datafiles directory, if not the default, so we can differentiate between db's of the same
              name in different places (for example temp ones on repair).
    */
    inline bool setClient(const char *ns, const string& path=dbpath, mongolock *lock = 0) {
        if( logLevel > 5 )
            log() << "setClient: " << ns << endl;

        dbMutex.assertAtLeastReadLocked();

        Client& c = cc();
        c.top.clientStart( ns );

        Database * db = dbHolder.get( ns , path );
        if ( db ){
            c.setns(ns, db );
            return false;
        }

        if( lock )
            lock->releaseAndWriteLock();

        assertInWriteLock();
        
        char cl[256];
        nsToDatabase(ns, cl);
        bool justCreated;
        Database *newdb = new Database(cl, justCreated, path);
        dbHolder.put(ns,path,newdb);
        c.setns(ns, newdb);

        newdb->finishInit();

        return justCreated;
    }

    // shared functionality for removing references to a database from this program instance
    // does not delete the files on disk
    void closeDatabase( const char *cl, const string& path = dbpath );

    inline bool clientIsEmpty() {
        return !cc().database()->namespaceIndex.allocated();
    }

    struct dbtemprelease {
        string clientname;
        string clientpath;
        int locktype;
        dbtemprelease() {
            Client& client = cc();
            Database *database = client.database();
            if ( database ) {
                clientname = database->name;
                clientpath = database->path;
            }
            client.top.clientStop();
            locktype = dbMutex.getState();
            assert( locktype );
            if ( locktype > 0 ) {
				massert( 10298 , "can't temprelease nested write lock", locktype == 1);
                dbMutex.unlock();
			}
            else {
				massert( 10299 , "can't temprelease nested read lock", locktype == -1);
                dbMutex.unlock_shared();
			}
        }
        ~dbtemprelease() {
            if ( locktype > 0 )
                dbMutex.lock();
            else
                dbMutex.lock_shared();
            if ( clientname.empty() )
                cc().setns("", 0);
            else
                setClient(clientname.c_str(), clientpath.c_str());
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
        
    };

    extern TicketHolder connTicketHolder;


} // namespace mongo

//#include "dbinfo.h"
#include "concurrency.h"
