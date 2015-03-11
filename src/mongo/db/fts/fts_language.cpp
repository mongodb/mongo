// fts_language.cpp

/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/db/fts/fts_language.h"

#include <string>
 
#include "mongo/base/init.h"
#include "mongo/db/fts/fts_basic_tokenizer.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    namespace fts {

        namespace {

            /**
             * Case-insensitive StringData comparator.
             */
            struct LanguageStringCompare {
                /** Returns true if lhs < rhs. */
                bool operator()( std::string lhs, std::string rhs ) const {
                    size_t minSize = std::min( lhs.size(), rhs.size() );

                    for ( size_t x = 0; x < minSize; x++ ) {
                        char a = tolower( lhs[x] );
                        char b = tolower( rhs[x] );
                        if ( a < b ) {
                            return true;
                        }
                        if ( a > b ) {
                            return false;
                        }
                    }

                    return lhs.size() < rhs.size();
                }
            };

            // Lookup table from user language string (case-insensitive) to FTSLanguage.  Populated
            // by initializers in group FTSAllLanguagesRegistered and initializer
            // FTSRegisterLanguageAliases.  For use with TEXT_INDEX_VERSION_2 text indexes only.
            typedef std::map<std::string, const FTSLanguage*, LanguageStringCompare> LanguageMapV2;
            LanguageMapV2 languageMapV2;

            // Like languageMapV2, but for use with TEXT_INDEX_VERSION_1 text indexes.
            // Case-sensitive by lookup key.
            typedef std::map<StringData, const FTSLanguage*> LanguageMapV1;
            LanguageMapV1 languageMapV1;
        }

        std::unique_ptr<FTSTokenizer> BasicFTSLanguage::createTokenizer() const {
            return stdx::make_unique<BasicFTSTokenizer>(this);
        }

        MONGO_INITIALIZER_GROUP( FTSAllLanguagesRegistered, MONGO_NO_PREREQUISITES,
                                 MONGO_NO_DEPENDENTS );

        //
        // Register supported languages' canonical names for TEXT_INDEX_VERSION_2.
        //

        MONGO_FTS_LANGUAGE_DECLARE( languageNoneV2, "none", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageDanishV2, "danish", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageDutchV2, "dutch", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageEnglishV2, "english", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageFinnishV2, "finnish", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageFrenchV2, "french", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageGermanV2, "german", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageHungarianV2, "hungarian", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageItalianV2, "italian", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageNorwegianV2, "norwegian", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languagePortugueseV2, "portuguese", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageRomanianV2, "romanian", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageRussianV2, "russian", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageSpanishV2, "spanish", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageSwedishV2, "swedish", TEXT_INDEX_VERSION_2 );
        MONGO_FTS_LANGUAGE_DECLARE( languageTurkishV2, "turkish", TEXT_INDEX_VERSION_2 );

        //
        // Register all Snowball language modules for TEXT_INDEX_VERSION_1.  Note that only the full
        // names are recognized by the StopWords class (as such, the language string "dan" in
        // TEXT_INDEX_VERSION_1 will generate the Danish stemmer and the empty stopword list).
        //

        MONGO_FTS_LANGUAGE_DECLARE( languageNoneV1, "none", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageDaV1, "da", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageDanV1, "dan", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageDanishV1, "danish", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageDeV1, "de", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageDeuV1, "deu", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageDutV1, "dut", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageDutchV1, "dutch", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageEnV1, "en", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageEngV1, "eng", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageEnglishV1, "english", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageEsV1, "es", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageEslV1, "esl", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageFiV1, "fi", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageFinV1, "fin", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageFinnishV1, "finnish", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageFrV1, "fr", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageFraV1, "fra", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageFreV1, "fre", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageFrenchV1, "french", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageGerV1, "ger", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageGermanV1, "german", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageHuV1, "hu", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageHunV1, "hun", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageHungarianV1, "hungarian", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageItV1, "it", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageItaV1, "ita", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageItalianV1, "italian", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageNlV1, "nl", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageNldV1, "nld", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageNoV1, "no", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageNorV1, "nor", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageNorwegianV1, "norwegian", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languagePorV1, "por", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languagePorterV1, "porter", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languagePortugueseV1, "portuguese", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languagePtV1, "pt", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageRoV1, "ro", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageRomanianV1, "romanian", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageRonV1, "ron", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageRuV1, "ru", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageRumV1, "rum", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageRusV1, "rus", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageRussianV1, "russian", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageSpaV1, "spa", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageSpanishV1, "spanish", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageSvV1, "sv", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageSweV1, "swe", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageSwedishV1, "swedish", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageTrV1, "tr", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageTurV1, "tur", TEXT_INDEX_VERSION_1 );
        MONGO_FTS_LANGUAGE_DECLARE( languageTurkishV1, "turkish", TEXT_INDEX_VERSION_1 );

        MONGO_INITIALIZER_WITH_PREREQUISITES( FTSRegisterLanguageAliases,
                                              ( "FTSAllLanguagesRegistered" ) )
                                            ( InitializerContext* context ) {
            // Register language aliases for TEXT_INDEX_VERSION_2.
            FTSLanguage::registerLanguageAlias( &languageDanishV2, "da", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languageDutchV2, "nl", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languageEnglishV2, "en", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languageFinnishV2, "fi", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languageFrenchV2, "fr", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languageGermanV2, "de", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languageHungarianV2, "hu", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languageItalianV2, "it", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languageNorwegianV2, "nb", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languagePortugueseV2, "pt", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languageRomanianV2, "ro", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languageRussianV2, "ru", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languageSpanishV2, "es", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languageSwedishV2, "sv", TEXT_INDEX_VERSION_2 );
            FTSLanguage::registerLanguageAlias( &languageTurkishV2, "tr", TEXT_INDEX_VERSION_2 );
            return Status::OK();
        }

        // static
        void FTSLanguage::registerLanguage( StringData languageName,
                                            TextIndexVersion textIndexVersion,
                                            FTSLanguage* language ) {
            verify( !languageName.empty() );
            language->_canonicalName = languageName.toString();
            switch ( textIndexVersion ) {
            case TEXT_INDEX_VERSION_2:
                languageMapV2[ languageName.toString() ] = language;
                return; 
            case TEXT_INDEX_VERSION_1:
                verify( languageMapV1.find( languageName ) == languageMapV1.end() );
                languageMapV1[ languageName ] = language;
                return;
            }
            verify( false );
        }

        // static
        void FTSLanguage::registerLanguageAlias( const FTSLanguage* language,
                                                 StringData alias,
                                                 TextIndexVersion textIndexVersion ) {
            switch ( textIndexVersion ) {
            case TEXT_INDEX_VERSION_2:
                languageMapV2[ alias.toString() ] = language;
                return;
            case TEXT_INDEX_VERSION_1:
                verify( languageMapV1.find( alias ) == languageMapV1.end() );
                languageMapV1[ alias ] = language;
                return;
            }
            verify( false );
        }

        FTSLanguage::FTSLanguage() : _canonicalName() {
        }

        const std::string& FTSLanguage::str() const {
            verify( !_canonicalName.empty() );
            return _canonicalName;
        }

        // static
        StatusWithFTSLanguage FTSLanguage::make( StringData langName,
                                                 TextIndexVersion textIndexVersion ) {
            switch ( textIndexVersion ) {
                case TEXT_INDEX_VERSION_2: {
                    LanguageMapV2::const_iterator it = languageMapV2.find( langName.toString() );
                    if ( it == languageMapV2.end() ) {
                        // TEXT_INDEX_VERSION_2 rejects unrecognized language strings.
                        Status status = Status( ErrorCodes::BadValue,
                                                mongoutils::str::stream() <<
                                                    "unsupported language: \"" << langName <<
                                                    "\"" );
                        return StatusWithFTSLanguage( status );
                    }

                    return StatusWithFTSLanguage( it->second );
                }
                case TEXT_INDEX_VERSION_1: {
                    LanguageMapV1::const_iterator it = languageMapV1.find( langName );
                    if ( it == languageMapV1.end() ) {
                        // TEXT_INDEX_VERSION_1 treats unrecognized language strings as "none".
                        return StatusWithFTSLanguage( &languageNoneV1 );
                    }
                    return StatusWithFTSLanguage( it->second );
                }
            }

            verify( false );
            return StatusWithFTSLanguage( Status::OK() );
        }
    }
}
