// tokenizer.h

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
*/


#pragma once

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    namespace fts {

        struct Token {
            enum Type { WHITESPACE, DELIMITER, TEXT, INVALID };
            Token( Type type, const StringData& data, unsigned offset, bool previousWhiteSpace )
                : type( type ),
                  data( data ),
                  offset( offset ),
                  previousWhiteSpace( previousWhiteSpace ) {}

            bool ok() const { return type != INVALID; }

            Type type;
            StringData data;
            unsigned offset;
            bool previousWhiteSpace;
        };

        class Tokenizer {
        public:

            Tokenizer( const std::string& language, const StringData& str );

            bool more() const;
            Token next();

        private:
            Token::Type _type( char c ) const;
            bool _skipWhitespace();

            unsigned _pos;
            bool _previousWhiteSpace;
            const StringData& _raw;
            bool _english;
        };

    }
}

