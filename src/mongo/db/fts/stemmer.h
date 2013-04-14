// stemmer.h

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
#include "third_party/libstemmer_c/include/libstemmer.h"

namespace mongo {

    namespace fts {

        /**
         * maintains case
         * but works
         * running/Running -> run/Run
         */
        class Stemmer {
        public:
            Stemmer( const std::string& language );
            ~Stemmer();

            std::string stem( const StringData& word ) const;
        private:
            struct sb_stemmer* _stemmer;
        };
    }
}

