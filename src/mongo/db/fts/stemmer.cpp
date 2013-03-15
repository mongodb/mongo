// stemmer.cpp

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

#include <cstdlib>
#include <string>

#include "mongo/db/fts/stemmer.h"

namespace mongo {

    namespace fts {

        Stemmer::Stemmer( const string& language ) {
            _stemmer = NULL;
            if ( language != "none" )
                _stemmer = sb_stemmer_new(language.c_str(), "UTF_8");
        }

        Stemmer::~Stemmer() {
            if ( _stemmer ) {
                sb_stemmer_delete(_stemmer);
                _stemmer = NULL;
            }
        }

        string Stemmer::stem( const StringData& word ) const {
            if ( !_stemmer )
                return word.toString();

            const sb_symbol* sb_sym = sb_stemmer_stem( _stemmer,
                                                       (const sb_symbol*)word.rawData(),
                                                       word.size() );

            if ( sb_sym == NULL ) {
                // out of memory
                abort();
            }

            return string( (const char*)(sb_sym), sb_stemmer_length( _stemmer ) );
        }

    }

}
