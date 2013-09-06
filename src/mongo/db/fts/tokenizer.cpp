// tokenizer.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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

#include <string>

#include "mongo/db/fts/tokenizer.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    namespace fts {

        Tokenizer::Tokenizer( const string& language, const StringData& str )
            : _pos(0), _raw( str ) {
            _english = language == "english";
            _skipWhitespace();
            _previousWhiteSpace = true;
        }

        bool Tokenizer::more() const {
            return _pos < _raw.size();
        }

        Token Tokenizer::next() {
            if ( _pos >= _raw.size() )
                return Token( Token::INVALID, "", 0, false );

            unsigned start = _pos++;
            Token::Type type = _type( _raw[start] );
            if ( type == Token::WHITESPACE ) abort();

            if ( type == Token::TEXT )
                while ( _pos < _raw.size() && _type( _raw[_pos] ) == type )
                    _pos++;

            StringData ret = _raw.substr( start, _pos - start );
            bool old = _previousWhiteSpace;
            _previousWhiteSpace = _skipWhitespace();
            return Token( type, ret, start, old );
        }


        bool Tokenizer::_skipWhitespace() {
            unsigned start = _pos;
            while ( _pos < _raw.size() && _type( _raw[_pos] ) == Token::WHITESPACE )
                _pos++;
            return _pos > start;
        }


        Token::Type Tokenizer::_type( char c ) const {
            switch ( c ) {
            case ' ':
            case '\f':
            case '\v':
            case '\t':
            case '\r':
            case '\n':
                return Token::WHITESPACE;
            case '\'':
                if ( _english )
                    return Token::TEXT;
                else
                    return Token::WHITESPACE;

            case '~':
            case '`':

            case '!':
            case '@':
            case '#':
            case '$':
            case '%':
            case '^':
            case '&':
            case '*':
            case '(':
            case ')':

            case '-':

            case '=':
            case '+':

            case '[':
            case ']':
            case '{':
            case '}':
            case '|':
            case '\\':

            case ';':
            case ':':

            case '"':

            case '<':
            case '>':

            case ',':
            case '.':

            case '/':
            case '?':

                return Token::DELIMITER;
            default:
                return Token::TEXT;
            }
        }

    }

}
