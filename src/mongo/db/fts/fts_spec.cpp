// fts_spec.cpp

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

#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    namespace fts {

        using namespace mongoutils;

        const double DEFAULT_WEIGHT = 1;
        const double MAX_WEIGHT = 1000000000;
        const double MAX_WORD_WEIGHT = MAX_WEIGHT / 10000;

        namespace {
            // Default language.  Used for new indexes.
            const std::string moduleDefaultLanguage( "english" );
        }

        FTSSpec::FTSSpec( const BSONObj& indexInfo ) {
            massert( 16739, "found invalid spec for text index",
                     indexInfo["weights"].isABSONObj() );

            Status status = _defaultLanguage.init( indexInfo["default_language"].String() );
            verify( status.isOK() );

            _languageOverrideField = indexInfo["language_override"].valuestrsafe();
            if ( _languageOverrideField.size() == 0 )
                _languageOverrideField = "language";

            _wildcard = false;

            // in this block we fill in the _weights map
            {
                BSONObjIterator i( indexInfo["weights"].Obj() );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    verify( e.isNumber() );

                    if ( WILDCARD == e.fieldName() ) {
                        _wildcard = true;
                    }
                    else {
                        double num = e.number();
                        _weights[ e.fieldName() ] = num;
                        verify( num > 0 && num < MAX_WORD_WEIGHT );
                    }
                }
                verify( _wildcard || _weights.size() );
            }

            // extra information
            {
                BSONObj keyPattern = indexInfo["key"].Obj();
                verify( keyPattern.nFields() >= 2 );
                BSONObjIterator i( keyPattern );

                bool passedFTS = false;

                while ( i.more() ) {
                    BSONElement e = i.next();
                    if ( str::equals( e.fieldName(), "_fts" ) ||
                         str::equals( e.fieldName(), "_ftsx" ) ) {
                        passedFTS = true;
                        continue;
                    }

                    if ( passedFTS )
                        _extraAfter.push_back( e.fieldName() );
                    else
                        _extraBefore.push_back( e.fieldName() );
                }

            }
        }

        const FTSLanguage FTSSpec::getLanguageToUse( const BSONObj& userDoc,
                                                     const FTSLanguage currentLanguage ) const {
            BSONElement e = userDoc[_languageOverrideField];
            if ( e.eoo() ) {
                return currentLanguage;
            }
            uassert( 17261,
                     "found language override field in document with non-string type",
                     e.type() == mongo::String );
            StatusWithFTSLanguage swl = FTSLanguage::makeFTSLanguage( e.String() );
            uassert( 17262,
                     "language override unsupported: " + e.String(),
                     swl.getStatus().isOK() );
            return swl.getValue();
        }



        namespace {
            /**
             * Check for exact match or path prefix match.
             */
            inline bool _matchPrefix( const string& dottedName, const string& weight ) {
                if ( weight == dottedName ) {
                    return true;
                }
                return str::startsWith( weight, dottedName + '.' );
            }
        }

        void FTSSpec::scoreDocument( const BSONObj& obj,
                                     const FTSLanguage parentLanguage,
                                     const string& parentPath,
                                     bool isArray,
                                     TermFrequencyMap* term_freqs ) const {
            const FTSLanguage language = getLanguageToUse( obj, parentLanguage );
            Stemmer stemmer( language );
            Tools tools( language, &stemmer, StopWords::getStopWords( language ) );

            // Perform a depth-first traversal of obj, skipping fields not touched by this spec.
            BSONObjIterator j( obj );
            while ( j.more() ) {

                BSONElement elem = j.next();
                string fieldName = elem.fieldName();

                // Skip "language" specifier fields if wildcard.
                if ( wildcard() && languageOverrideField() == fieldName ) {
                    continue;
                }

                // Compose the dotted name of the current field:
                // 1. parent path empty (top level): use the current field name
                // 2. parent path non-empty and obj is an array: use the parent path
                // 3. parent path non-empty and obj is a sub-doc: append field name to parent path
                string dottedName = ( parentPath.empty() ? fieldName
                                          : isArray ? parentPath
                                          : parentPath + '.' + fieldName );

                // Find lower bound of dottedName in _weights.  lower_bound leaves us at the first
                // weight that could possibly match or be a prefix of dottedName.  And if this
                // element fails to match, then no subsequent weight can match, since the weights
                // are lexicographically ordered.
                Weights::const_iterator i = _weights.lower_bound( dottedName );

                // possibleWeightMatch is set if the weight map contains either a match or some item
                // lexicographically larger than fieldName.  This boolean acts as a guard on
                // dereferences of iterator 'i'.
                bool possibleWeightMatch = ( i != _weights.end() );

                // Optimize away two cases, when not wildcard:
                // 1. lower_bound seeks to end(): no prefix match possible
                // 2. lower_bound seeks to a name which is not a prefix
                if ( !wildcard() ) {
                    if ( !possibleWeightMatch ) {
                        continue;
                    }
                    else if ( !_matchPrefix( dottedName, i->first ) ) {
                        continue;
                    }
                }

                // Is the current field an exact match on a weight?
                bool exactMatch = ( possibleWeightMatch && i->first == dottedName );

                double weight = ( possibleWeightMatch ? i->second : DEFAULT_WEIGHT );

                switch ( elem.type() ) {
                case String:
                    // Only index strings on exact match or wildcard.
                    if ( exactMatch || wildcard() ) {
                        _scoreString( tools, elem.valuestr(), term_freqs, weight );
                    }
                    break;
                case Object:
                    // Only descend into a sub-document on proper prefix or wildcard.  Note that
                    // !exactMatch is a sufficient test for proper prefix match, because of
                    // matchPrefix() continue block above.
                    if ( !exactMatch || wildcard() ) {
                        scoreDocument( elem.Obj(), language, dottedName, false, term_freqs );
                    }
                    break;
                case Array:
                    // Only descend into arrays from non-array parents or on wildcard.
                    if ( !isArray || wildcard() ) {
                        scoreDocument( elem.Obj(), language, dottedName, true, term_freqs );
                    }
                    break;
                default:
                    // Skip over all other BSON types.
                    break;
                }
            }
        }

        namespace {
            struct ScoreHelperStruct {
                ScoreHelperStruct()
                    : freq(0), count(0), exp(0){
                }
                double freq;
                double count;
                double exp;
            };
            typedef unordered_map<string,ScoreHelperStruct> ScoreHelperMap;
        }

        void FTSSpec::_scoreString( const Tools& tools,
                                    const StringData& raw,
                                    TermFrequencyMap* docScores,
                                    double weight ) const {

            ScoreHelperMap terms;

            unsigned numTokens = 0;

            Tokenizer i( tools.language, raw );
            while ( i.more() ) {
                Token t = i.next();
                if ( t.type != Token::TEXT )
                    continue;

                string term = t.data.toString();
                makeLower( &term );
                if ( tools.stopwords->isStopWord( term ) )
                    continue;
                term = tools.stemmer->stem( term );

                ScoreHelperStruct& data = terms[term];

                if ( data.exp )
                    data.exp *= 2;
                else
                    data.exp = 1;
                data.count += 1;
                data.freq += ( 1 / data.exp );

                numTokens++;
            }

            for ( ScoreHelperMap::const_iterator i = terms.begin(); i != terms.end(); ++i ) {

                const string& term = i->first;
                const ScoreHelperStruct& data = i->second;

                // in order to adjust weights as a function of term count as it
                // relates to total field length. ie. is this the only word or
                // a frequently occuring term? or does it only show up once in
                // a long block of text?

                double coeff = ( 0.5 * data.count / numTokens ) + 0.5;

                // if term is identical to the raw form of the
                // field (untokenized) give it a small boost.
                double adjustment = 1;
                if ( raw.size() == term.length() && raw.equalCaseInsensitive( term ) )
                    adjustment += 0.1;

                double& score = (*docScores)[term];
                score += ( weight * data.freq * coeff * adjustment );
                verify( score <= MAX_WEIGHT );
            }
        }

        Status FTSSpec::getIndexPrefix( const BSONObj& query, BSONObj* out ) const {
            if ( numExtraBefore() == 0 ) {
                *out = BSONObj();
                return Status::OK();
            }

            BSONObjBuilder b;
            for ( unsigned i = 0; i < numExtraBefore(); i++ ) {
                BSONElement e = query.getFieldDotted(extraBefore(i));
                if ( e.eoo() )
                    return Status( ErrorCodes::BadValue,
                                   str::stream()
                                   << "need have an equality filter on: "
                                   << extraBefore(i) );

                if ( e.isABSONObj() && e.Obj().firstElement().getGtLtOp( -1 ) != -1 )
                    return Status( ErrorCodes::BadValue,
                                   str::stream()
                                   << "need have an equality filter on: "
                                   << extraBefore(i) );

                b.append( e );
            }
            *out = b.obj();
            return Status::OK();
        }

        void _addFTSStuff( BSONObjBuilder* b ) {
            b->append( "_fts", INDEX_NAME );
            b->append( "_ftsx", 1 );
        }

        BSONObj FTSSpec::fixSpec( const BSONObj& spec ) {
            map<string,int> m;

            BSONObj keyPattern;
            {
                BSONObjBuilder b;
                bool addedFtsStuff = false;

                BSONObjIterator i( spec["key"].Obj() );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    if ( str::equals( e.fieldName(), "_fts" ) ||
                         str::equals( e.fieldName(), "_ftsx" ) ) {
                        addedFtsStuff = true;
                        b.append( e );
                    }
                    else if ( e.type() == String &&
                              ( str::equals( "fts", e.valuestr() ) ||
                                str::equals( "text", e.valuestr() ) ) ) {

                        if ( !addedFtsStuff ) {
                            _addFTSStuff( &b );
                            addedFtsStuff = true;
                        }

                        m[e.fieldName()] = 1;
                    }
                    else {
                        b.append( e );
                    }
                }

                if ( !addedFtsStuff )
                    _addFTSStuff( &b );

                keyPattern = b.obj();
            }

            if ( spec["weights"].isABSONObj() ) {
                BSONObjIterator i( spec["weights"].Obj() );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    m[e.fieldName()] = e.numberInt();
                }
            }
            else if ( spec["weights"].str() == WILDCARD ) {
                m[WILDCARD] = 1;
            }

            BSONObj weights;
            {
                BSONObjBuilder b;
                for ( map<string,int>::iterator i = m.begin(); i != m.end(); ++i ) {
                    uassert( 16674, "score for word too high",
                             i->second > 0 && i->second < MAX_WORD_WEIGHT );
                    b.append( i->first, i->second );
                }
                weights = b.obj();
            }

            BSONElement default_language_elt = spec["default_language"];
            string default_language( default_language_elt.str() );
            if ( default_language_elt.eoo() ) {
                default_language = moduleDefaultLanguage;
            }
            else {
                uassert( 17263,
                         "default_language needs a string type",
                         default_language_elt.type() == String );
            }
            uassert( 17264,
                     "default_language is not valid",
                     FTSLanguage::makeFTSLanguage( default_language ).getStatus().isOK() );

            string language_override(spec.getStringField("language_override"));
            if ( language_override.empty() )
                language_override = "language";

            int version = -1;
            int textIndexVersion = 2;

            BSONObjBuilder b;
            BSONObjIterator i( spec );
            while ( i.more() ) {
                BSONElement e = i.next();
                if ( str::equals( e.fieldName(), "key" ) ) {
                    b.append( "key", keyPattern );
                }
                else if ( str::equals( e.fieldName(), "weights" ) ) {
                    b.append( "weights", weights );
                    weights = BSONObj();
                }
                else if ( str::equals( e.fieldName(), "default_language" ) ) {
                    b.append( "default_language", default_language);
                    default_language = "";
                }
                else if ( str::equals( e.fieldName(), "language_override" ) ) {
                    b.append( "language_override", language_override);
                    language_override = "";
                }
                else if ( str::equals( e.fieldName(), "v" ) ) {
                    version = e.numberInt();
                }
                else if ( str::equals( e.fieldName(), "textIndexVersion" ) ) {
                    textIndexVersion = e.numberInt();
                    uassert( 16730,
                             str::stream() << "bad textIndexVersion: " << textIndexVersion,
                             textIndexVersion == 2 );
                }
                else {
                    b.append( e );
                }
            }

            if ( !weights.isEmpty() )
                b.append( "weights", weights );
            if ( !default_language.empty() )
                b.append( "default_language", default_language);
            if ( !language_override.empty() )
                b.append( "language_override", language_override);

            if ( version >= 0 )
                b.append( "v", version );

            b.append( "textIndexVersion", textIndexVersion );

            return b.obj();

        }

    }
}
