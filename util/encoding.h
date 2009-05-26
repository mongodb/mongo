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

namespace mongo {
    
    string latin1ToUtf8( const string &in ) {
        stringstream out;
        for( size_t i = 0; i < in.size(); ++i ) {
            unsigned char c = in[ i ];
            if ( c < 0x80 ) {
                out << char( c );
            } else {
                out << char( 0xc0 | ( c >> 6 ) );
                out << char( 0x80 | ( ~0xc0 & c ) ); 
            }
        }
        return out.str();
    }
    
    string utf8ToLatin1( const string &in ) {
        stringstream out;
        for( size_t i = 0; i < in.size(); ++i ) {
            unsigned char c = in[ i ];
            if ( c < 0x80 ) {
                out << char( c );
            } else if ( c < 0xC4 ) {
                unsigned char first = c;
                ++i;
                massert( "invalid utf8 input", i < in.size() );
                unsigned char second = in[ i ];
                massert( "invalid utf8 input", second < 0xC0 );
                out << char( ( first << 6 ) | ( ~0xc0 & second ) );
            } else {
                out << "?";
                for( ; i < in.size() && (unsigned char)in[ i ] > 0x7F; ++i );
            }
        }
        return out.str();        
    }
    
} // namespace mongo