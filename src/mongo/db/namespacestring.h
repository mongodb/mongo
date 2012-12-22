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

#include "mongo/util/assert_util.h"

namespace mongo {

    using std::string;

    /* in the mongo source code, "client" means "database". */

    const size_t MaxDatabaseNameLen = 128; // max str len for the db name, including null char

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

        /**
         * @return true if the namespace is valid. Special namespaces for internal use are considered as valid.
         */
        bool isValid() const {
            return validDBName( db ) && !coll.empty();
        }

        operator string() const { return ns(); }

        bool operator==( const string& nsIn ) const { return nsIn == ns(); }
        bool operator==( const char* nsIn ) const { return (string)nsIn == ns(); }
        bool operator==( const NamespaceString& nsIn ) const { return nsIn.db == db && nsIn.coll == coll; }

        bool operator!=( const string& nsIn ) const { return nsIn != ns(); }
        bool operator!=( const char* nsIn ) const { return (string)nsIn != ns(); }
        bool operator!=( const NamespaceString& nsIn ) const { return nsIn.db != db || nsIn.coll != coll; }

        size_t size() const { return ns().size(); }

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
#ifdef _WIN32
            // We prohibit all FAT32-disallowed characters on Windows
            size_t good = strcspn( db.c_str() , "/\\. \"*<>:|?" );
#else
            // For non-Windows platforms we are much more lenient
            size_t good = strcspn( db.c_str() , "/\\. \"" );
#endif
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
            const char *c = strchr( dbcoll, '.' );
            return (c != NULL) && (c[1] != '\0') && normal(dbcoll);
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
    inline StringData nsToDatabaseSubstring( const StringData& ns ) {
        size_t i = ns.find( '.' );
        if ( i == string::npos ) {
            massert(10078, "nsToDatabase: ns too long", ns.size() < MaxDatabaseNameLen );
            return ns;
        }
        massert(10088, "nsToDatabase: ns too long", i < static_cast<size_t>(MaxDatabaseNameLen));
        return ns.substr( 0, i );
    }

    // "database.a.b.c" -> "database"
    inline void nsToDatabase(const StringData& ns, char *database) {
        StringData db = nsToDatabaseSubstring( ns );
        db.copyTo( database, true );
    }

    // TODO: make this return a StringData
    inline string nsToDatabase(const StringData& ns) {
        return nsToDatabaseSubstring( ns ).toString();
    }

    /**
     * NamespaceDBHash and NamespaceDBEquals allow you to do something like
     * unordered_map<string,int,NamespaceDBHash,NamespaceDBEquals>
     * and use the full namespace for the string
     * but comparisons are done only on the db piece
     */
    
    /**
     * this can change, do not store on disk
     */
    inline int nsDBHash( const string& ns ) {
        int hash = 7;
        for ( size_t i = 0; i < ns.size(); i++ ) {
            if ( ns[i] == '.' )
                break;
            hash += 11 * ( ns[i] );
            hash *= 3;
        }
        return hash;
    }

    inline bool nsDBEquals( const string& a, const string& b ) {
        for ( size_t i = 0; i < a.size(); i++ ) {
            
            if ( a[i] == '.' ) {
                // b has to either be done or a '.'
                
                if ( b.size() == i )
                    return true;

                if ( b[i] == '.' ) 
                    return true;

                return false;
            }
            
            // a is another character
            if ( b.size() == i )
                return false;
            
            if ( b[i] != a[i] )
                    return false;
        }
        
        // a is done
        // make sure b is done 
        if ( b.size() == a.size() || 
             b[a.size()] == '.' )
            return true;

        return false;
    }
    
    struct NamespaceDBHash {
        int operator()( const string& ns ) const {
            return nsDBHash( ns );
        }
    };

    struct NamespaceDBEquals {
        bool operator()( const string& a, const string& b ) const {
            return nsDBEquals( a, b );
        }
    };
    
}
