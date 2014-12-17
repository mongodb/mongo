// string_data_inline.h

/*    Copyright 2010 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

// this should never be included directly

#include <stdexcept>

namespace mongo {

    inline int StringData::compare(const StringData& other) const {
        int res = memcmp(_data, other._data, std::min(_size, other._size));
        if (res != 0) {
            return res > 0 ? 1 : -1;
        }
        else if (_size == other._size) {
            return 0;
        }
        else {
            return _size > other._size ? 1 : -1;
        }
    }

    inline bool StringData::equalCaseInsensitive( const StringData& other ) const {
        if ( other.size() != size() )
            return false;

        for ( size_t x = 0; x < size(); x++ ) {
            char a = _data[x];
            char b = other._data[x];
            if ( a == b )
                continue;
            if ( tolower(a) == tolower(b) )
                continue;
            return false;
        }

        return true;
    }

    inline void StringData::copyTo( char* dest, bool includeEndingNull ) const {
        memcpy( dest, _data, size() );
        if ( includeEndingNull )
            dest[size()] = 0;
    }

    inline size_t StringData::find( char c, size_t fromPos ) const {
        if ( fromPos >= size() )
            return std::string::npos;

        const void* x = memchr( _data + fromPos, c, _size - fromPos );
        if ( x == 0 )
            return std::string::npos;
        return static_cast<size_t>( static_cast<const char*>(x) - _data );
    }

    inline size_t StringData::find( const StringData& needle ) const {
        size_t mx = size();
        size_t needleSize = needle.size();

        if ( needleSize == 0 )
            return 0;
        else if ( needleSize > mx )
            return std::string::npos;

        mx -= needleSize;

        for ( size_t i = 0; i <= mx; i++ ) {
            if ( memcmp( _data + i, needle._data, needleSize ) == 0 )
                return i;
        }
        return std::string::npos;

    }

    inline size_t StringData::rfind( char c, size_t fromPos ) const {
        const size_t sz = size();
        if ( fromPos > sz )
            fromPos = sz;

        for ( const char* cur = _data + fromPos; cur > _data; --cur ) {
            if ( *(cur - 1) == c )
                return (cur - _data) - 1;
        }
        return std::string::npos;
    }

    inline StringData StringData::substr( size_t pos, size_t n ) const {
        if ( pos > size() )
            throw std::out_of_range( "out of range" );

        // truncate to end of string
        if ( n > size() - pos )
            n = size() - pos;

        return StringData( _data + pos, n );
    }

    inline bool StringData::startsWith( const StringData& prefix ) const {
        // TODO: Investigate an optimized implementation.
        return substr(0, prefix.size()) == prefix;
    }

    inline bool StringData::endsWith( const StringData& suffix ) const {
        // TODO: Investigate an optimized implementation.
        const size_t thisSize = size();
        const size_t suffixSize = suffix.size();
        if (suffixSize > thisSize)
            return false;
        return substr(thisSize - suffixSize) == suffix;
    }

}  // namespace mongo
