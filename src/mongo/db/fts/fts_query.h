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

