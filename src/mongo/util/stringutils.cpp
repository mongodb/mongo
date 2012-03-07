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

#include "stringutils.h"

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
    
    LexNumCmp::LexNumCmp( bool lexOnly ) :
    _lexOnly( lexOnly ) {
    }

    int LexNumCmp::cmp( const char *s1, const char *s2, bool lexOnly ) {
        //cout << "START : " << s1 << "\t" << s2 << endl;
        
        bool startWord = true;
        
        while( *s1 && *s2 ) {
            
            bool d1 = ( *s1 == '.' );
            bool d2 = ( *s2 == '.' );
            if ( d1 && !d2 )
                return -1;
            if ( d2 && !d1 )
                return 1;
            if ( d1 && d2 ) {
                ++s1; ++s2;
                startWord = true;
                continue;
            }
            
            bool p1 = ( *s1 == (char)255 );
            bool p2 = ( *s2 == (char)255 );
            //cout << "\t\t " << p1 << "\t" << p2 << endl;
            if ( p1 && !p2 )
                return 1;
            if ( p2 && !p1 )
                return -1;
            
            if ( !lexOnly ) {
                
                bool n1 = isNumber( *s1 );
                bool n2 = isNumber( *s2 );
                
                if ( n1 && n2 ) {
                    // get rid of leading 0s
                    if ( startWord ) {
                        while ( *s1 == '0' ) s1++;
                        while ( *s2 == '0' ) s2++;
                    }
                    
                    char * e1 = (char*)s1;
                    char * e2 = (char*)s2;
                    
                    // find length
                    // if end of string, will break immediately ('\0')
                    while ( isNumber (*e1) ) e1++;
                    while ( isNumber (*e2) ) e2++;
                    
                    int len1 = (int)(e1-s1);
                    int len2 = (int)(e2-s2);
                    
                    int result;
                    // if one is longer than the other, return
                    if ( len1 > len2 ) {
                        return 1;
                    }
                    else if ( len2 > len1 ) {
                        return -1;
                    }
                    // if the lengths are equal, just strcmp
                    else if ( (result = strncmp(s1, s2, len1)) != 0 ) {
                        return result;
                    }
                    
                    // otherwise, the numbers are equal
                    s1 = e1;
                    s2 = e2;
                    startWord = false;
                    continue;
                }
                
                if ( n1 )
                    return 1;
                
                if ( n2 )
                    return -1;
            }
            
            if ( *s1 > *s2 )
                return 1;
            
            if ( *s2 > *s1 )
                return -1;
            
            s1++; s2++;
            startWord = false;
        }
        
        if ( *s1 )
            return 1;
        if ( *s2 )
            return -1;
        return 0;
    }

    int LexNumCmp::cmp( const char *s1, const char *s2 ) const {
        return cmp( s1, s2, _lexOnly );
    }
    bool LexNumCmp::operator()( const char *s1, const char *s2 ) const {
        return cmp( s1, s2 ) < 0;
    }
    bool LexNumCmp::operator()( const string &s1, const string &s2 ) const {
        return (*this)( s1.c_str(), s2.c_str() );
    }
    
} // namespace mongo
