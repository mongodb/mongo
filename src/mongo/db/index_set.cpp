// index_set.h

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

#include "mongo/bson/util/builder.h"
#include "mongo/db/index_set.h"

namespace mongo {

    void IndexPathSet::addPath( const StringData& path ) {
        string s;
        if ( getCanonicalIndexField( path, &s ) ) {
            _canonical.push_back( s );
        }
        else {
            _canonical.push_back( path.toString() );
        }
    }

    void IndexPathSet::clear() {
        _canonical.clear();
    }

    bool IndexPathSet::mightBeIndexed( const StringData& path ) const {
        StringData use = path;
        string x;
        if ( getCanonicalIndexField( path, &x ) )
            use = StringData(x);

        for ( unsigned i = 0; i < _canonical.size(); i++ ) {
            StringData idx( _canonical[i] );

            if ( use.startsWith( idx ) )
                return true;

            if ( idx.startsWith( use ) )
                return true;
        }

        return false;
    }

    bool getCanonicalIndexField( const StringData& fullName, string* out ) {
        // check if fieldName contains ".$" or ".###" substrings (#=digit) and skip them
        if ( fullName.find( '.' ) == string::npos )
            return false;

        bool modified = false;

        StringBuilder buf;
        for ( size_t i=0; i<fullName.size(); i++ ) {

            char c = fullName[i];

            if ( c != '.' ) {
                buf << c;
                continue;
            }

            // check for ".$", skip if present
            if ( fullName[i+1] == '$' ) {
                i++;
                modified = true;
                continue;
            }

            // check for ".###" for any number of digits (no letters)
            if ( isdigit( fullName[i+1] ) ) {
                size_t j = i;
                // skip digits
                while ( j+1 < fullName.size() && isdigit( fullName[j+1] ) )
                    j++;

                if ( j+1 == fullName.size() || fullName[j+1] == '.' ) {
                    // only digits found, skip forward
                    i = j;
                    modified = true;
                    continue;
                }
            }

            buf << c;
        }

        if ( !modified )
            return false;

        *out = buf.str();
        return true;
    }


}

#if 0
        /**
         * checks if mod is in the index by inspecting fieldName, and removing
         * .$ or .### substrings (#=digit) with any number of digits.
         *
         * @return true iff the mod is indexed
         */
        bool isIndexed( const set<string>& idxKeys ) const {

            // first, check if full name is in idxKeys
            if ( isIndexed( fieldName , idxKeys ) )
                return true;

            string x;
            if ( getCanonicalIndexField( fieldName, &x ) ) {
                if ( isIndexed( x, idxKeys ) )
                    return true;
            }

            return false;
        }


        static bool isIndexed( const string& fullName , const set<string>& idxKeys ) {
            const char * fieldName = fullName.c_str();
            // check if there is an index key that is a parent of mod
            for( const char* dot = strchr( fieldName, '.' ); dot; dot = strchr( dot + 1, '.' ) )
                if ( idxKeys.count( string( fieldName, dot - fieldName ) ) )
                    return true;

            // check if there is an index key equal to mod
            if ( idxKeys.count(fullName) )
                return true;

            return false;
        }
#endif
