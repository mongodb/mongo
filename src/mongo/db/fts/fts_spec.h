// fts_spec.h

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

#include <map>
#include <vector>
#include <string>

#include "mongo/db/fts/fts_util.h"
#include "mongo/db/fts/stemmer.h"
#include "mongo/db/fts/stop_words.h"
#include "mongo/db/fts/tokenizer.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    namespace fts {

        extern const double MAX_WEIGHT;

        typedef std::map<string,double> Weights; // TODO cool map

        typedef unordered_map<string,double> TermFrequencyMap;


        class FTSSpec {

            struct Tools {
                Tools( string language )
                    : language( language ){}
                const std::string& language;
                const Stemmer* stemmer;
                const StopWords* stopwords;
            };

        public:
            FTSSpec( const BSONObj& indexInfo );

            bool wildcard() const { return _wildcard; }
            const string& defaultLanguage() const { return _defaultLanguage; }
            const string& languageOverrideField() const { return _languageOverrideField; }

            size_t numExtraBefore() const { return _extraBefore.size(); }
            const std::string& extraBefore( unsigned i ) const { return _extraBefore[i]; }

            size_t numExtraAfter() const { return _extraAfter.size(); }
            const std::string& extraAfter( unsigned i ) const { return _extraAfter[i]; }

            string getLanguageToUse( const BSONObj& userDoc ) const;

            void scoreDocument( const BSONObj& obj, TermFrequencyMap* scores ) const;

            /**
             * given a query, pulls out the pieces (in order) that go in the index first
             */
            Status getIndexPrefix( const BSONObj& filter, BSONObj* out ) const;

            const Weights& weights() const { return _weights; }

            /**
             * @param out - untouched if field isn't present
             * @return if field is here
             */
            bool weight( const StringData& field, double* out ) const;


            static BSONObj fixSpec( const BSONObj& spec );
        private:
            void _scoreRecurse(const Tools& tools,
                               const BSONObj& obj,
                               TermFrequencyMap* term_freqs ) const;

            void _scoreString( const Tools& tools,
                               const StringData& raw,
                               TermFrequencyMap* term_freqs,
                               double weight ) const;

            string _defaultLanguage;
            string _languageOverrideField;
            bool _wildcard;

            // _weights stores a mapping between the fields and the value as a double
            // basically, how much should an occurence of (query term) in (field) be worth
            Weights _weights;

            // other fields to index
            std::vector<string> _extraBefore;
            std::vector<string> _extraAfter;
        };

    }
}
