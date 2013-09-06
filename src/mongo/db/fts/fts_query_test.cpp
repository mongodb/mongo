// fts_query_test.cpp

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


#include "mongo/db/fts/fts_query.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
    namespace fts {

        TEST( FTSQuery, Basic1 ) {
            FTSQuery q;
            ASSERT( q.parse( "this is fun", "english" ).isOK() );

            ASSERT_EQUALS( 1U, q.getTerms().size() );
            ASSERT_EQUALS( "fun", q.getTerms()[0] );
            ASSERT_EQUALS( 0U, q.getNegatedTerms().size() );
            ASSERT_EQUALS( 0U, q.getPhr().size() );
            ASSERT_EQUALS( 0U, q.getNegatedPhr().size() );
        }

        TEST( FTSQuery, Neg1 ) {
            FTSQuery q;
            ASSERT( q.parse( "this is -really fun", "english" ).isOK() );

            ASSERT_EQUALS( 1U, q.getTerms().size() );
            ASSERT_EQUALS( "fun", q.getTerms()[0] );
            ASSERT_EQUALS( 1U, q.getNegatedTerms().size() );
            ASSERT_EQUALS( "realli", *q.getNegatedTerms().begin() );
        }

        TEST( FTSQuery, Phrase1 ) {
            FTSQuery q;
            ASSERT( q.parse( "doing a \"phrase test\" for fun", "english" ).isOK() );

            ASSERT_EQUALS( 3U, q.getTerms().size() );
            ASSERT_EQUALS( 0U, q.getNegatedTerms().size() );
            ASSERT_EQUALS( 1U, q.getPhr().size() );
            ASSERT_EQUALS( 0U, q.getNegatedPhr().size() );

            ASSERT_EQUALS( "phrase test", q.getPhr()[0] );
            ASSERT_EQUALS( "fun|phrase|test||||phrase test||", q.debugString() );
        }

        TEST( FTSQuery, Phrase2 ) {
            FTSQuery q;
            ASSERT( q.parse( "doing a \"phrase-test\" for fun", "english" ).isOK() );
            ASSERT_EQUALS( 1U, q.getPhr().size() );
            ASSERT_EQUALS( "phrase-test", q.getPhr()[0] );
        }

        TEST( FTSQuery, NegPhrase1 ) {
            FTSQuery q;
            ASSERT( q.parse( "doing a -\"phrase test\" for fun", "english" ).isOK() );
            ASSERT_EQUALS( "fun||||||phrase test", q.debugString() );
        }

        TEST( FTSQuery, Mix1 ) {
            FTSQuery q;
            ASSERT( q.parse( "\"industry\" -Melbourne -Physics", "english" ).isOK() );
            ASSERT_EQUALS( "industri||melbourn|physic||industry||", q.debugString() );
        }

        TEST( FTSQuery, NegPhrase2) {
            FTSQuery q1, q2, q3;
            ASSERT( q1.parse( "foo \"bar\"", "english" ).isOK() );
            ASSERT( q2.parse( "foo \"-bar\"", "english" ).isOK() );
            ASSERT( q3.parse( "foo \" -bar\"", "english" ).isOK() );

            ASSERT_EQUALS( 2U, q1.getTerms().size() );
            ASSERT_EQUALS( 2U, q2.getTerms().size() );
            ASSERT_EQUALS( 2U, q3.getTerms().size() );

            ASSERT_EQUALS( 0U, q1.getNegatedTerms().size() );
            ASSERT_EQUALS( 0U, q2.getNegatedTerms().size() );
            ASSERT_EQUALS( 0U, q3.getNegatedTerms().size() );

            ASSERT_EQUALS( 1U, q1.getPhr().size() );
            ASSERT_EQUALS( 1U, q2.getPhr().size() );
            ASSERT_EQUALS( 1U, q3.getPhr().size() );

            ASSERT_EQUALS( 0U, q1.getNegatedPhr().size() );
            ASSERT_EQUALS( 0U, q2.getNegatedPhr().size() );
            ASSERT_EQUALS( 0U, q3.getNegatedPhr().size() );
        }

        TEST( FTSQuery, NegPhrase3) {
            FTSQuery q1, q2, q3;
            ASSERT( q1.parse( "foo -\"bar\"", "english" ).isOK() );
            ASSERT( q2.parse( "foo -\"-bar\"", "english" ).isOK() );
            ASSERT( q3.parse( "foo -\" -bar\"", "english" ).isOK() );

            ASSERT_EQUALS( 1U, q1.getTerms().size() );
            ASSERT_EQUALS( 1U, q2.getTerms().size() );
            ASSERT_EQUALS( 1U, q3.getTerms().size() );

            ASSERT_EQUALS( 0U, q1.getNegatedTerms().size() );
            ASSERT_EQUALS( 0U, q2.getNegatedTerms().size() );
            ASSERT_EQUALS( 0U, q3.getNegatedTerms().size() );

            ASSERT_EQUALS( 0U, q1.getPhr().size() );
            ASSERT_EQUALS( 0U, q2.getPhr().size() );
            ASSERT_EQUALS( 0U, q3.getPhr().size() );

            ASSERT_EQUALS( 1U, q1.getNegatedPhr().size() );
            ASSERT_EQUALS( 1U, q2.getNegatedPhr().size() );
            ASSERT_EQUALS( 1U, q3.getNegatedPhr().size() );
        }

    }
}
