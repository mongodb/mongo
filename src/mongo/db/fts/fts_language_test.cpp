// fts_language_test.cpp

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

#include "mongo/pch.h"
#include "mongo/db/fts/fts_language.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    namespace fts {

        // Positive tests for FTSLanguage::init() and FTSLanguage::str().

        TEST( FTSLanguage, ExactLanguage ) {
            FTSLanguage lang;
            Status s = lang.init( "spanish" );
            ASSERT( s.isOK() );
            ASSERT_EQUALS( lang.str(), "spanish" );
        }

        TEST( FTSLanguage, ExactCode ) {
            FTSLanguage lang;
            Status s = lang.init( "es" );
            ASSERT( s.isOK() );
            ASSERT_EQUALS( lang.str(), "spanish" );
        }

        TEST( FTSLanguage, UpperCaseLanguage ) {
            FTSLanguage lang;
            Status s = lang.init( "SPANISH" );
            ASSERT( s.isOK() );
            ASSERT_EQUALS( lang.str(), "spanish" );
        }

        TEST( FTSLanguage, UpperCaseCode ) {
            FTSLanguage lang;
            Status s = lang.init( "ES" );
            ASSERT( s.isOK() );
            ASSERT_EQUALS( lang.str(), "spanish" );
        }
        
        TEST( FTSLanguage, NoneLanguage ) {
            FTSLanguage lang;
            Status s = lang.init( "none" );
            ASSERT( s.isOK() );
            ASSERT_EQUALS( lang.str(), "none" );
        }

        // Negative tests for FTSLanguage::init() and FTSLanguage::str().

        TEST( FTSLanguage, Unknown ) {
            FTSLanguage lang;
            Status s = lang.init( "spanglish" );
            ASSERT( !s.isOK() );
        }

        TEST( FTSLanguage, Empty ) {
            FTSLanguage lang;
            Status s = lang.init( "" );
            ASSERT( !s.isOK() );
        }

        // Positive tests for FTSLanguage::makeFTSLanguage().

        TEST( FTSLanguage, MakeFTSLanguage1 ) {
            StatusWithFTSLanguage swl = FTSLanguage::makeFTSLanguage( "english" );
            ASSERT( swl.getStatus().isOK() );
            ASSERT_EQUALS( swl.getValue().str(), "english" );
        }

        // Negative tests for FTSLanguage::makeFTSLanguage().

        TEST( FTSLanguage, MakeFTSLanguage2 ) {
            StatusWithFTSLanguage swl = FTSLanguage::makeFTSLanguage( "onglish" );
            ASSERT( !swl.getStatus().isOK() );
        }

    }
}
