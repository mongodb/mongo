// fts_matcher.h

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

#include "mongo/db/fts/fts_query.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/tokenizer.h"

namespace mongo {

    namespace fts {

        class FTSMatcher {
        public:
            FTSMatcher( const FTSQuery& query, const FTSSpec& spec );

            /**
             * @return true if obj has a negated term
             */
            bool hasNegativeTerm(const BSONObj& obj ) const;

            /**
             * @return true if obj is ok by all phrases
             *         so all full phrases and no negated
             */
            bool phrasesMatch( const BSONObj& obj ) const;

            bool phraseMatch( const string& phrase, const BSONObj& obj ) const;

            bool matchesNonTerm( const BSONObj& obj ) const {
                return !hasNegativeTerm( obj ) && phrasesMatch( obj );
            }

        private:
            bool _hasNegativeTerm_recurse(const BSONObj& obj ) const;

            /**
             * @return true if raw has a negated term
             */
            bool _hasNegativeTerm_string( const string& raw ) const;

            bool _phraseRecurse( const string& phrase, const BSONObj& obj ) const;
            bool _phraseMatches( const string& phrase, const string& haystack ) const;

            FTSQuery _query;
            FTSSpec _spec;
            Stemmer _stemmer;
        };

    }
}
