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

    void splitStringDelim( const string& str, vector<string>& vec, char delim ){
        string s(str);

        while ( true ){
            size_t idx = s.find( delim );
            if ( idx == string::npos ){
                vec.push_back( s );
                break;
            }
            vec.push_back( s.substr( 0 , idx ) );
            s = s.substr( idx + 1 );
        }
    }


} // namespace mongo
