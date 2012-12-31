// fts_query.h

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
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/fts/stemmer.h"
#include "mongo/db/fts/stop_words.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    namespace fts {

        using std::string;
        using std::vector;
        using std::set;

        class FTSQuery {

        public:
            Status parse(const string& query, const string& language);

            const vector<string>& getTerms() const { return _terms; }
            const unordered_set<string>& getNegatedTerms() const { return _negatedTerms; }

            const vector<string>& getPhr() const { return _phrases; }
            const vector<string>& getNegatedPhr() const { return _negatedPhrases; }

            /**
             * @return true if any negations or phrase + or -
             */
            bool hasNonTermPieces() const {
                return
                    _negatedTerms.size() > 0 ||
                    _phrases.size() > 0 ||
                    _negatedPhrases.size() > 0;
            }

            string getSearch() const { return _search; }
            string getLanguage() const { return _language; }

            string toString() const;

            string debugString() const;

        protected:
            string _search;
            string _language;
            vector<string> _terms;
            unordered_set<string> _negatedTerms;
            vector<string> _phrases;
            vector<string> _negatedPhrases;

        private:
            void _addTerm( const StopWords* sw, Stemmer& stemmer, const string& term, bool negated );
        };

    }
}

