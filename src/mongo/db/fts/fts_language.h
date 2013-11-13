// fts_language.h

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

#pragma once

#include "mongo/base/status_with.h"

#include <string>

namespace mongo {

    namespace fts {

        /**
         * A FTSLanguage is a copyable glorified enum representing a language for a text-indexed
         * document or a text search.  Example of suggested usage:
         *
         *     StatusWithFTSLanguage swl = FTSLanguage::makeFTSLanguage( "en" );
         *     if ( !swl.getStatus().isOK() ) {
         *         // Error.
         *     }
         *     else {
         *         const FTSLanguage language = swl.getValue();
         *         // Use language.
         *     }
         */
        class FTSLanguage {
        public:
            /** Create an uninitialized language. */
            FTSLanguage();

            ~FTSLanguage();
            FTSLanguage( const FTSLanguage& );
            FTSLanguage& operator=( const FTSLanguage & );

            /**
             * Initialize an FTSLanguage from a language string.  Language strings are
             * case-insensitive, and can be in one of the two following forms:
             * - English name, like "spanish".
             * - Two-letter code, like "es".
             * Returns an error Status if an invalid language string is passed.
             */
            Status init( const std::string& lang );

            /**
             * Returns the language as a string in canonical form (lowercased English name).  It is
             * an error to call str() on an uninitialized language.
             */
            std::string str() const;

            /**
             * Convenience method for creating an FTSLanguage out of a language string.  Caller
             * must check getStatus().isOK() on return value. 
             */
            static StatusWith<const FTSLanguage> makeFTSLanguage( const std::string& lang );

        private:
            // Pointer to string representation of language.  Not owned here.
            StringData _lang;
        };

        typedef StatusWith<const FTSLanguage> StatusWithFTSLanguage;

    }
}
