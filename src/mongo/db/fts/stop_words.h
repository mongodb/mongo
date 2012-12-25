// stop_words.h

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

#include <set>
#include <string>

#include "mongo/platform/unordered_set.h"

namespace mongo {

    namespace fts {

        class StopWords {
        public:
            StopWords();
            StopWords( const std::set<std::string>& words );

            bool isStopWord( const std::string& word ) const {
                return _words.count( word ) > 0;
            }

            size_t numStopWords() const { return _words.size(); }

            static const StopWords* getStopWords( const std::string& langauge );
        private:
            ~StopWords(){}
            unordered_set<std::string> _words;
        };

    }
}

