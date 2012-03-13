// @file namespacestring.h

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

#include <string>

namespace mongo {

    using std::string;

    /* in the mongo source code, "client" means "database". */

    const int MaxDatabaseNameLen = 128; // max str len for the db name, including null char

    /* e.g.
       NamespaceString ns("acme.orders");
       cout << ns.coll; // "orders"
    */
    class NamespaceString {
    public:
        string db;
        string coll; // note collection names can have periods in them for organizing purposes (e.g. "system.indexes")

        NamespaceString( const char * ns ) { init(ns); }
        NamespaceString( const string& ns ) { init(ns.c_str()); }

        string ns() const { return db + '.' + coll; }

        bool isSystem() const { return strncmp(coll.c_str(), "system.", 7) == 0; }
        bool isCommand() const { return coll == "$cmd"; }

        operator string() const { return ns(); }

        bool operator==( const string& nsIn ) const { return nsIn == ns(); }
        bool operator==( const char* nsIn ) const { return (string)nsIn == ns(); }
        bool operator==( const NamespaceString& nsIn ) const { return nsIn.db == db && nsIn.coll == coll; }

        bool operator!=( const string& nsIn ) const { return nsIn != ns(); }
        bool operator!=( const char* nsIn ) const { return (string)nsIn != ns(); }
        bool operator!=( const NamespaceString& nsIn ) const { return nsIn.db != db || nsIn.coll != coll; }

        string toString() const { return ns(); }

        /**
         * @return true if ns is 'normal'.  $ used for collections holding index data, which do not contain BSON objects in their records.
         * special case for the local.oplog.$main ns -- naming it as such was a mistake.
         */
        static bool normal(const char* ns) {
            const char *p = strchr(ns, '$');
            if( p == 0 )
                return true;
            return strcmp( ns, "local.oplog.$main" ) == 0;
        }

        static bool special(const char *ns) { 
            return !normal(ns) || strstr(ns, ".system.");
        }

        /**
         * samples:
         *   good:  
         *      foo  
         *      bar
         *      foo-bar
         *   bad:
         *      foo bar
         *      foo.bar
         *      foo"bar
         *        
         * @param db - a possible database name
         * @return if db is an allowed database name
         */
        static bool validDBName( const string& db ) {
            if ( db.size() == 0 || db.size() > 64 )
                return false;
            size_t good = strcspn( db.c_str() , "/\\. \"*<>:|?" );
            return good == db.size();
        }

        /**
         * samples:
         *   good:
         *      foo.bar
         *   bad:
         *      foo.
         *
         * @param dbcoll - a possible collection name of the form db.coll
         * @return if db.coll is an allowed collection name
         */
        static bool validCollectionName(const char* dbcoll){
          const char *c = strchr( dbcoll, '.' ) + 1;
          return normal(dbcoll) && c && *c;
        }

    private:
        void init(const char *ns) {
            const char *p = strchr(ns, '.');
            if( p == 0 ) return;
            db = string(ns, p - ns);
            coll = p + 1;
        }
    };

    // "database.a.b.c" -> "database"
    inline void nsToDatabase(const char *ns, char *database) {
        for( int i = 0; i < MaxDatabaseNameLen; i++ ) {
            database[i] = ns[i];
            if( database[i] == '.' ) {
                database[i] = 0;
                return;
            }
            if( database[i] == 0 ) {
                return;
            }
        }
        // other checks should have happened already, this is defensive. thus massert not uassert
        massert(10078, "nsToDatabase: ns too long", false);
    }
    inline string nsToDatabase(const char *ns) {
        char buf[MaxDatabaseNameLen];
        nsToDatabase(ns, buf);
        return buf;
    }
    inline string nsToDatabase(const string& ns) {
        size_t i = ns.find( '.' );
        if ( i == string::npos )
            return ns;
        massert(10088, "nsToDatabase: ns too long", i < (size_t)MaxDatabaseNameLen);
        return ns.substr( 0 , i );
    }

}
