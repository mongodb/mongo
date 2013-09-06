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

            if ( _query.getNegatedTerms().size() == 0 )
                return false;

            if ( _spec.wildcard() ) {
                return _hasNegativeTerm_recurse(obj);
            }

            /* otherwise look at fields where weights are defined */
            for ( Weights::const_iterator i = _spec.weights().begin();
                  i != _spec.weights().end();
                  i++ ) {
                const char * leftOverName = i->first.c_str();
                BSONElement e = obj.getFieldDottedOrArray(leftOverName);

                if ( e.type() == Array ) {
                    BSONObjIterator j( e.Obj() );
                    while ( j.more() ) {
                        BSONElement x = j.next();
                        if ( leftOverName[0] && x.isABSONObj() )
                            x = x.Obj().getFieldDotted( leftOverName );
                        if ( x.type() == String )
                            if ( _hasNegativeTerm_string( x.String() ) )
                                return true;
                    }
                }
                else if ( e.type() == String ) {
                    if ( _hasNegativeTerm_string( e.String() ) )
                        return true;
                }
            }
            return false;
        }

        bool FTSMatcher::_hasNegativeTerm_recurse(const BSONObj& obj ) const {
            BSONObjIterator j( obj );
            while ( j.more() ) {
                BSONElement x = j.next();

                if ( _spec.languageOverrideField() == x.fieldName())
                    continue;

                if (x.type() == String) {
                    if ( _hasNegativeTerm_string( x.String() ) )
                        return true;
                }
                else if ( x.isABSONObj() ) {
                    BSONObjIterator k( x.Obj() );
                    while ( k.more() ) {
                        // check if k.next() is a obj/array or not
                        BSONElement y = k.next();
                        if ( y.type() == String ) {
                            if ( _hasNegativeTerm_string( y.String() ) )
                                return true;
                        }
                        else if ( y.isABSONObj() ) {
                            if ( _hasNegativeTerm_recurse( y.Obj() ) )
                                return true;
                        }
                    }
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
                string word = tolowerString( _stemmer.stem( t.data ) );
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

            if ( _spec.wildcard() ) {
                // case where everything is indexed (all fields)
                return _phraseRecurse( phrase, obj );
            }

            for ( Weights::const_iterator i = _spec.weights().begin();
                  i != _spec.weights().end();
                  ++i ) {

                //  figure out what the indexed field is.. ie. is it "field" or "field.subfield" etc.
                const char * leftOverName = i->first.c_str();
                BSONElement e = obj.getFieldDottedOrArray(leftOverName);

                if ( e.type() == Array ) {
                    BSONObjIterator j( e.Obj() );
                    while ( j.more() ) {
                        BSONElement x = j.next();

                        if ( leftOverName[0] && x.isABSONObj() )
                            x = x.Obj().getFieldDotted( leftOverName );

                        if ( x.type() == String )
                            if ( _phraseMatches( phrase, x.String() ) )
                                return true;
                    }
                }
                else if ( e.type() == String ) {
                    if ( _phraseMatches( phrase, e.String() ) )
                        return true;
                }
            }
            return false;
        }


        /*
         * Recurses over all fields in the obj to match against phrase
         * @param phrase, string to be matched
         * @param obj, object to matched against
         */
        bool FTSMatcher::_phraseRecurse( const string& phrase, const BSONObj& obj ) const {
            BSONObjIterator j( obj );
            while ( j.more() ) {
                BSONElement x = j.next();

                if ( _spec.languageOverrideField() == x.fieldName() )
                    continue;

                if ( x.type() == String ) {
                    if ( _phraseMatches( phrase, x.String() ) )
                        return true;
                } 
                else if ( x.isABSONObj() ) {
                    BSONObjIterator k( x.Obj() );

                    while ( k.more() ) {

                        BSONElement y = k.next();

                        if ( y.type() == mongo::String ) {
                            if ( _phraseMatches( phrase, y.String() ) )
                                return true;
                        }
                        else if ( y.isABSONObj() ) {
                            if ( _phraseRecurse( phrase, y.Obj() ) )
                                return true;
                        }
                    }

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
