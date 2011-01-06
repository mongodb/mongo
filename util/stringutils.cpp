// stringutils.cpp

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

#include "pch.h"

namespace mongo {

    void splitStringDelim( const string& str , vector<string>* res , char delim ) {
        if ( str.empty() )
            return;

        size_t beg = 0;
        size_t pos = str.find( delim );
        while ( pos != string::npos ) {
            res->push_back( str.substr( beg, pos - beg) );
            beg = ++pos;
            pos = str.find( delim, beg );
        }
        res->push_back( str.substr( beg ) );
    }

    void joinStringDelim( const vector<string>& strs , string* res , char delim ) {
        for ( vector<string>::const_iterator it = strs.begin(); it != strs.end(); ++it ) {
            if ( it !=strs.begin() ) res->push_back( delim );
            res->append( *it );
        }
    }

} // namespace mongo
