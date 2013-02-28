// string_data_inline.h

/*    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

// this should never be included directly

#include <stdexcept>

namespace mongo {

    inline int StringData::compare(const StringData& other) const {
        // Sizes might not have been computed yet.
        size();
        other.size();

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
            return string::npos;

        const void* x = memchr( _data + fromPos, c, _size - fromPos );
        if ( x == 0 )
            return string::npos;
        return static_cast<size_t>( static_cast<const char*>(x) - _data );
    }

    inline size_t StringData::find( const StringData& needle ) const {
        size_t mx = size();
        size_t needleSize = needle.size();

        if ( needleSize == 0 )
            return 0;
        else if ( needleSize > mx )
            return string::npos;

        mx -= needleSize;

        for ( size_t i = 0; i <= mx; i++ ) {
            if ( memcmp( _data + i, needle._data, needleSize ) == 0 )
                return i;
        }
        return string::npos;

    }

    inline size_t StringData::rfind( char c, size_t fromPos ) const {
        const size_t sz = size();
        if ( fromPos > sz )
            fromPos = sz;

        for ( const char* cur = _data + fromPos; cur > _data; --cur ) {
            if ( *(cur - 1) == c )
                return (cur - _data) - 1;
        }
        return string::npos;
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
