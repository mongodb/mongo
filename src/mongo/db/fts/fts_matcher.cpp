// fts_matcher.cpp

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

#include "mongo/pch.h"

#include "mongo/db/fts/fts_matcher.h"
#include "mongo/db/fts/fts_element_iterator.h"
#include "mongo/platform/strcasestr.h"

namespace mongo {

    namespace fts {

        FTSMatcher::FTSMatcher( const FTSQuery& query, const FTSSpec& spec )
            : _query( query ),
              _spec( spec ),
              _stemmer( query.getLanguage() ){
        }

        /*
         * Checks if the obj contains any of the negTerms, if so returns true, otherwise false
         * @param obj, object to be checked
         */
        bool FTSMatcher::hasNegativeTerm(const BSONObj& obj ) const {
            // called during search. deals with the case in which we have a term
            // flagged for exclusion, i.e. "hello -world" we want to remove all
            // results that include "world"

            if ( _query.getNegatedTerms().size() == 0 ) {
                return false;
            }

            FTSElementIterator it( _spec, obj);

            while ( it.more() ) {
                FTSIteratorValue val = it.next();
                if (_hasNegativeTerm_string( val._text )) {
                    return true;
                }
            }

            return false;
        }

        /*
         * Checks if any of the negTerms is in the tokenized string
         * @param raw, the raw string to be tokenized
         */
        bool FTSMatcher::_hasNegativeTerm_string( const string& raw ) const {

            Tokenizer i( _query.getLanguage(), raw );
            while ( i.more() ) {
                Token t = i.next();
                if ( t.type != Token::TEXT )
                    continue;
                string word = _stemmer.stem( tolowerString( t.data ) );
                if ( _query.getNegatedTerms().count( word ) > 0 )
                    return true;
            }
            return false;
        }

        bool FTSMatcher::phrasesMatch( const BSONObj& obj ) const {
            for (unsigned i = 0; i < _query.getPhr().size(); i++ ) {
                if ( !phraseMatch( _query.getPhr()[i], obj ) ) {
                    return false;
                }
            }

            for (unsigned i = 0; i < _query.getNegatedPhr().size(); i++ ) {
                if ( phraseMatch( _query.getNegatedPhr()[i], obj ) ) {
                    return false;
                }
            }

            return true;
        }

        /**
         * Checks if phrase is exactly matched in obj, returns true if so, false otherwise
         * @param phrase, the string to be matched
         * @param obj, document in the collection to match against
         */
        bool FTSMatcher::phraseMatch( const string& phrase, const BSONObj& obj ) const {
            FTSElementIterator it( _spec, obj);

            while ( it.more() ) {
                FTSIteratorValue val = it.next();
                if (_phraseMatches( phrase, val._text )) {
                    return true;
                }
            }

            return false;
        }

        /*
         * Looks for phrase in a raw string
         * @param phrase, phrase to match
         * @param haystack, raw string to be parsed
         */
        bool FTSMatcher::_phraseMatches( const string& phrase, const string& haystack ) const {
            return strcasestr( haystack.c_str(), phrase.c_str() ) > 0;
        }
    }
}
