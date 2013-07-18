// database_holder.cpp

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
*/

#include "mongo/pch.h"

#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/client.h"
#include "mongo/db/database_holder.h"

namespace mongo {

    Database* DatabaseHolder::getOrCreate( const string& ns, const string& path, bool& justCreated ) {
        string dbname = _todb( ns );
        {
            SimpleMutex::scoped_lock lk(_m);
            Lock::assertAtLeastReadLocked(ns);
            DBs& m = _paths[path];
            {
                DBs::iterator i = m.find(dbname);
                if( i != m.end() ) {
                    justCreated = false;
                    return i->second;
                }
            }

            // todo: protect against getting sprayed with requests for different db names that DNE -
            //       that would make the DBs map very large.  not clear what to do to handle though,
            //       perhaps just log it, which is what we do here with the "> 40" :
            bool cant = !Lock::isWriteLocked(ns);
            if( logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1)) ||
                m.size() > 40 || cant || DEBUG_BUILD ) {
                log() << "opening db: " << (path==dbpath?"":path) << ' ' << dbname << endl;
            }
            massert(15927, "can't open database in a read lock. if db was just closed, consider retrying the query. might otherwise indicate an internal error", !cant);
        }

        // we mark our thread as having done writes now as we do not want any exceptions
        // once we start creating a new database
        cc().writeHappened();

        // this locks _m for defensive checks, so we don't want to be locked right here :
        Database *db = new Database( dbname.c_str() , justCreated , path );

        {
            SimpleMutex::scoped_lock lk(_m);
            DBs& m = _paths[path];
            verify( m[dbname] == 0 );
            m[dbname] = db;
            _size++;
        }

        authindex::configureSystemIndexes(dbname);

        db->clearTmpCollections();

        return db;
    }
}
