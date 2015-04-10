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

#include <algorithm>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    /* in the mongo source code, "client" means "database". */

    const size_t MaxDatabaseNameLen = 128; // max str len for the db name, including null char

    /** @return true if a client can modify this namespace even though it is under ".system."
        For example <dbname>.system.users is ok for regular clients to update.
        @param write used when .system.js
    */
    bool legalClientSystemNS( StringData ns , bool write );

    /* e.g.
       NamespaceString ns("acme.orders");
       cout << ns.coll; // "orders"
    */
    class NamespaceString {
    public:
        /**
         * Constructs an empty NamespaceString.
         */
        NamespaceString();

        /**
         * Constructs a NamespaceString from the fully qualified namespace named in "ns".
         */
        explicit NamespaceString( StringData ns );

        /**
         * Constructs a NamespaceString for the given database and collection names.
         * "dbName" must not contain a ".", and "collectionName" must not start with one.
         */
        NamespaceString( StringData dbName, StringData collectionName );

        /**
         * Note that these values are derived from the mmap_v1 implementation and that
         * is the only reason they are constrained as such.
         */
        enum MaxNsLenValue {
            // Maximum possible length of name any namespace, including special ones like $extra.
            // This includes rum for the NUL byte so it can be used when sizing buffers.
            MaxNsLenWithNUL = 128,

            // MaxNsLenWithNUL excluding the NUL byte. Use this when comparing std::string lengths.
            MaxNsLen = MaxNsLenWithNUL - 1,

            // Maximum allowed length of fully qualified namespace name of any real collection.
            // Does not include NUL so it can be directly compared to std::string lengths.
            MaxNsCollectionLen = MaxNsLen - 7/*strlen(".$extra")*/,
        };

        StringData db() const;
        StringData coll() const;

        const std::string& ns() const { return _ns; }

        operator const std::string&() const { return ns(); }
        const std::string& toString() const { return ns(); }

        size_t size() const { return _ns.size(); }

        //
        // The following methods assume isValid() is true for this NamespaceString.
        //

        bool isSystem() const { return coll().startsWith( "system." ); }
        bool isSystemDotIndexes() const { return coll() == "system.indexes"; }
        bool isSystemDotProfile() const { return coll() == "system.profile"; }
        bool isConfigDB() const { return db() == "config"; }
        bool isCommand() const { return coll() == "$cmd"; }
        bool isOplog() const { return oplog( _ns ); }
        bool isSpecialCommand() const { return coll().startsWith("$cmd.sys"); }
        bool isSpecial() const { return special( _ns ); }
        bool isNormal() const { return normal( _ns ); }
        bool isListCollectionsGetMore() const;
        bool isListIndexesGetMore() const;

        /**
         * Given a NamespaceString for which isListIndexesGetMore() returns true, returns the
         * NamespaceString for the collection that the "listIndexesGetMore" targets.
         */
        NamespaceString getTargetNSForListIndexesGetMore() const;

        /**
         * @return true if the namespace is valid. Special namespaces for internal use are considered as valid.
         */
        bool isValid() const { return validDBName( db() ) && !coll().empty(); }

        bool operator==( const std::string& nsIn ) const { return nsIn == _ns; }
        bool operator==( StringData nsIn ) const { return nsIn == _ns; }
        bool operator==( const NamespaceString& nsIn ) const { return nsIn._ns == _ns; }

        bool operator!=( const std::string& nsIn ) const { return nsIn != _ns; }
        bool operator!=( const NamespaceString& nsIn ) const { return nsIn._ns != _ns; }

        bool operator<( const NamespaceString& rhs ) const { return _ns < rhs._ns; }

        /** ( foo.bar ).getSisterNS( "blah" ) == foo.blah
         */
        std::string getSisterNS( StringData local ) const;

        // @return db() + ".system.indexes"
        std::string getSystemIndexesCollection() const;

        // @return db() + ".$cmd"
        std::string getCommandNS() const;

        /**
         * @return true if ns is 'normal'.  A "$" is used for namespaces holding index data,
         * which do not contain BSON objects in their records. ("oplog.$main" is the exception)
         */
        static bool normal(StringData ns);

        /**
         * @return true if the ns is an oplog one, otherwise false.
         */
        static bool oplog(StringData ns);

        static bool special(StringData ns);

        /**
         * samples:
         *   good
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
        static bool validDBName( StringData dbin );

        /**
         * Takes a fully qualified namespace (ie dbname.collectionName), and returns true if
         * the collection name component of the namespace is valid.
         * samples:
         *   good:
         *      foo.bar
         *   bad:
         *      foo.
         *
         * @param ns - a full namespace (a.b)
         * @return if db.coll is an allowed collection name
         */
        static bool validCollectionComponent(StringData ns);

        /**
         * Takes a collection name and returns true if it is a valid collection name.
         * samples:
         *   good:
         *     foo
         *     system.indexes
         *   bad:
         *     $foo
         * @param coll - a collection name component of a namespace
         * @return if the input is a valid collection name
         */
        static bool validCollectionName(StringData coll);

    private:

        std::string _ns;
        size_t _dotIndex;
    };


    // "database.a.b.c" -> "database"
    inline StringData nsToDatabaseSubstring( StringData ns ) {
        size_t i = ns.find( '.' );
        if ( i == std::string::npos ) {
            massert(10078, "nsToDatabase: db too long", ns.size() < MaxDatabaseNameLen );
            return ns;
        }
        massert(10088, "nsToDatabase: db too long", i < static_cast<size_t>(MaxDatabaseNameLen));
        return ns.substr( 0, i );
    }

    // "database.a.b.c" -> "database"
    inline void nsToDatabase(StringData ns, char *database) {
        StringData db = nsToDatabaseSubstring( ns );
        db.copyTo( database, true );
    }

    // TODO: make this return a StringData
    inline std::string nsToDatabase(StringData ns) {
        return nsToDatabaseSubstring( ns ).toString();
    }

    // "database.a.b.c" -> "a.b.c"
    inline StringData nsToCollectionSubstring( StringData ns ) {
        size_t i = ns.find( '.' );
        massert(16886, "nsToCollectionSubstring: no .", i != std::string::npos );
        return ns.substr( i + 1 );
    }

    /**
     * foo = false
     * foo. = false
     * foo.a = true
     */
    inline bool nsIsFull( StringData ns ) {
        size_t i = ns.find( '.' );
        if ( i == std::string::npos )
            return false;
        if ( i == ns.size() - 1 )
            return false;
        return true;
    }

    /**
     * foo = true
     * foo. = false
     * foo.a = false
     */
    inline bool nsIsDbOnly(StringData ns) {
        size_t i = ns.find('.');
        if (i == std::string::npos)
            return true;
        return false;
    }

    /**
     * NamespaceDBHash and NamespaceDBEquals allow you to do something like
     * unordered_map<std::string,int,NamespaceDBHash,NamespaceDBEquals>
     * and use the full namespace for the string
     * but comparisons are done only on the db piece
     */

    /**
     * this can change, do not store on disk
     */
    int nsDBHash( const std::string& ns );

    bool nsDBEquals( const std::string& a, const std::string& b );

    struct NamespaceDBHash {
        int operator()( const std::string& ns ) const {
            return nsDBHash( ns );
        }
    };

    struct NamespaceDBEquals {
        bool operator()( const std::string& a, const std::string& b ) const {
            return nsDBEquals( a, b );
        }
    };

}


#include "mongo/db/namespace_string-inl.h"
