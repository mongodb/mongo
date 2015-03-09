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
#include "mongo/util/stringutils.h"

namespace mongo {

    namespace fts {

        class FTSQuery {

        public:
            // Initializes an FTSQuery.  Note that the parsing of "language" depends on the text
            // index version, since a query which doesn't specify a language and is against a
            // version 1 text index with a version 1 default language string needs to be parsed as
            // version 1 (see fts_language.cpp for a list of language strings specific to version
            // 1).
            Status parse(const std::string& query, StringData language,
                         TextIndexVersion textIndexVersion);

            const std::vector<std::string>& getTerms() const { return _terms; }
            const std::set<std::string>& getNegatedTerms() const { return _negatedTerms; }

            const std::vector<std::string>& getPhr() const { return _phrases; }
            const std::vector<std::string>& getNegatedPhr() const { return _negatedPhrases; }

            /**
             * @return true if any negations or phrase + or -
             */
            bool hasNonTermPieces() const {
                return
                    _negatedTerms.size() > 0 ||
                    _phrases.size() > 0 ||
                    _negatedPhrases.size() > 0;
            }

            const FTSLanguage& getLanguage() const { return *_language; }

            std::string toString() const;

            std::string debugString() const;

            BSONObj toBSON() const;

        protected:
            const FTSLanguage* _language;
            std::vector<std::string> _terms;
            std::set<std::string> _negatedTerms;
            std::vector<std::string> _phrases;
            std::vector<std::string> _negatedPhrases;

        private:
            void _addTerm( const StopWords* sw, Stemmer& stemmer, const std::string& term, bool negated );
        };

    }
}

