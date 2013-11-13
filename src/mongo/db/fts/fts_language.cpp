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
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    namespace fts {

        namespace {

            // Supported languages in canonical form (English names, lowercased).  Includes "none".
            const string LanguageNone( "none" );
            const string LanguageDanish( "danish" );
            const string LanguageDutch( "dutch" );
            const string LanguageEnglish( "english" );
            const string LanguageFinnish( "finnish" );
            const string LanguageFrench( "french" );
            const string LanguageGerman( "german" );
            const string LanguageHungarian( "hungarian" );
            const string LanguageItalian( "italian" );
            const string LanguageNorwegian( "norwegian" );
            const string LanguagePortuguese( "portuguese" );
            const string LanguageRomanian( "romanian" );
            const string LanguageRussian( "russian" );
            const string LanguageSpanish( "spanish" );
            const string LanguageSwedish( "swedish" );
            const string LanguageTurkish( "turkish" );

            // Map from lowercased user string to language string.  Resolves any language aliases
            // (two-letter codes).
            typedef StringMap<std::string> LanguageMap;
            LanguageMap languageMap;
        }

        MONGO_INITIALIZER( FTSLanguageMap )( InitializerContext* context ) {
            languageMap[LanguageNone] = LanguageNone;

            languageMap["da"] = LanguageDanish;
            languageMap[LanguageDanish] = LanguageDanish;
            languageMap["nl"] = LanguageDutch;
            languageMap[LanguageDutch] = LanguageDutch;
            languageMap["en"] = LanguageEnglish;
            languageMap[LanguageEnglish] = LanguageEnglish;
            languageMap["fi"] = LanguageFinnish;
            languageMap[LanguageFinnish] = LanguageFinnish;
            languageMap["fr"] = LanguageFrench;
            languageMap[LanguageFrench] = LanguageFrench;
            languageMap["de"] = LanguageGerman;
            languageMap[LanguageGerman] = LanguageGerman;
            languageMap["hu"] = LanguageHungarian;
            languageMap[LanguageHungarian] = LanguageHungarian;
            languageMap["it"] = LanguageItalian;
            languageMap[LanguageItalian] = LanguageItalian;
            languageMap["nb"] = LanguageNorwegian;
            languageMap[LanguageNorwegian] = LanguageNorwegian;
            languageMap["pt"] = LanguagePortuguese;
            languageMap[LanguagePortuguese] = LanguagePortuguese;
            languageMap["ro"] = LanguageRomanian;
            languageMap[LanguageRomanian] = LanguageRomanian;
            languageMap["ru"] = LanguageRussian;
            languageMap[LanguageRussian] = LanguageRussian;
            languageMap["es"] = LanguageSpanish;
            languageMap[LanguageSpanish] = LanguageSpanish;
            languageMap["sv"] = LanguageSwedish;
            languageMap[LanguageSwedish] = LanguageSwedish;
            languageMap["tr"] = LanguageTurkish;
            languageMap[LanguageTurkish] = LanguageTurkish;
            return Status::OK();
        }

        FTSLanguage::FTSLanguage()
            : _lang() {
        }

        FTSLanguage::FTSLanguage( const FTSLanguage& other )
            : _lang( other._lang ) {
        }

        FTSLanguage& FTSLanguage::operator=( const FTSLanguage& other ) {
            _lang = other._lang;
            return *this;
        }

        FTSLanguage::~FTSLanguage() {
        }

        Status FTSLanguage::init( const std::string& lang ) {
            // Lowercase.
            std::string langLower = tolowerString( lang );

            // Resolve language aliases.
            LanguageMap::const_iterator it = languageMap.find( langLower );
            if ( it == languageMap.end() ) {
                return Status( ErrorCodes::BadValue,
                               "unsupported language: \"" + lang + "\"" );
            }

            _lang = StringData( it->second );
            return Status::OK();
        }

        std::string FTSLanguage::str() const {
            verify( !_lang.empty() );
            return _lang.toString();
        }

        StatusWithFTSLanguage FTSLanguage::makeFTSLanguage( const std::string& lang ) {
            FTSLanguage language;
            Status s = language.init( lang );
            if ( !s.isOK() ) {
                return StatusWithFTSLanguage( s );
            }
            return StatusWithFTSLanguage( language );
        }

    }
}
