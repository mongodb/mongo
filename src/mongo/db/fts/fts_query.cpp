// fts_query.cpp

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

#include "mongo/pch.h"

#include "mongo/db/fts/fts_query.h"
#include "mongo/db/fts/tokenizer.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    namespace fts {

        using namespace mongoutils;

        Status FTSQuery::parse(const string& query, const string& language) {
            _search = query;
            _language = language;

            const StopWords* stopWords = StopWords::getStopWords( language );
            Stemmer stemmer( language );

            bool inNegation = false;
            bool inPhrase = false;

            unsigned quoteOffset = 0;

            Tokenizer i( _language, query );
            while ( i.more() ) {
                Token t = i.next();

                if ( t.type == Token::TEXT ) {
                    string s = t.data.toString();

                    if ( inPhrase && inNegation ) {
                        // don't add term
                    }
                    else {
                        _addTerm( stopWords, stemmer, s, inNegation );
                    }

                    if ( inNegation && !inPhrase )
                        inNegation = false;
                }
                else if ( t.type == Token::DELIMITER ) {
                    char c = t.data[0];
                    if ( c == '-' ) {
                        if ( t.previousWhiteSpace )
                            inNegation = true;
                    }
                    else if ( c == '"' ) {
                        if ( inPhrase ) {
                            // end of a phrase
                            unsigned phraseStart = quoteOffset + 1;
                            unsigned phraseLength = t.offset - phraseStart;
                            StringData phrase = StringData( query ).substr( phraseStart,
                                                                            phraseLength );
                            if ( inNegation )
                                _negatedPhrases.push_back( tolowerString( phrase ) );
                            else
                                _phrases.push_back( tolowerString( phrase ) );
                            inNegation = false;
                            inPhrase = false;
                        }
                        else {
                            // start of a phrase
                            inPhrase = true;
                            quoteOffset = t.offset;
                        }
                    }
                }
                else {
                    abort();
                }
            }

            return Status::OK();
        }

        void FTSQuery::_addTerm( const StopWords* sw, Stemmer& stemmer, const string& term, bool negated ) {
            string word = tolowerString( term );
            if ( sw->isStopWord( word ) )
                return;
            word = stemmer.stem( word );
            if ( negated )
                _negatedTerms.insert( word );
            else
                _terms.push_back( word );
        }

        namespace {
            void _debugHelp( stringstream& ss, const set<string>& s, const string& sep ) {
                bool first = true;
                for ( set<string>::const_iterator i = s.begin(); i != s.end(); ++i ) {
                    if ( first )
                        first = false;
                    else
                        ss << sep;
                    ss << *i;
                }
            }

            void _debugHelp( stringstream& ss, const vector<string>& v, const string& sep ) {
                set<string> s( v.begin(), v.end() );
                _debugHelp( ss, s, sep );
            }

            void _debugHelp( stringstream& ss, const unordered_set<string>& v, const string& sep ) {
                set<string> s( v.begin(), v.end() );
                _debugHelp( ss, s, sep );
            }

        }

        string FTSQuery::toString() const {
            stringstream ss;
            ss << "FTSQuery\n";

            ss << "  terms: ";
            _debugHelp( ss, getTerms(), ", " );
            ss << "\n";

            ss << "  negated terms: ";
            _debugHelp( ss, getNegatedTerms(), ", " );
            ss << "\n";

            ss << "  phrases: ";
            _debugHelp( ss, getPhr(), ", " );
            ss << "\n";

            ss << "  negated phrases: ";
            _debugHelp( ss, getNegatedPhr(), ", " );
            ss << "\n";

            return ss.str();
        }

        string FTSQuery::debugString() const {
            stringstream ss;

            _debugHelp( ss, getTerms(), "|" );
            ss << "||";

            _debugHelp( ss, getNegatedTerms(), "|" );
            ss << "||";

            _debugHelp( ss, getPhr(), "|" );
            ss << "||";

            _debugHelp( ss, getNegatedPhr(), "|" );

            return ss.str();
        }
    }
}
