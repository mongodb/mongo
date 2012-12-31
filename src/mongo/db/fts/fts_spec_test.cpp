// fts_spec_test.cpp

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

#include "mongo/db/fts/fts_spec.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
    namespace fts {

        TEST( FTSSpec, Fix1 ) {
            BSONObj user = BSON( "key" << BSON( "title" << "fts" <<
                                                "text" << "fts" ) <<
                                 "weights" << BSON( "title" << 10 ) );

            BSONObj fixed = FTSSpec::fixSpec( user );
            BSONObj fixed2 = FTSSpec::fixSpec( fixed );
            ASSERT_EQUALS( fixed, fixed2 );
        }

        TEST( FTSSpec, ScoreSingleField1 ) {
            BSONObj user = BSON( "key" << BSON( "title" << "fts" <<
                                                "text" << "fts" ) <<
                                 "weights" << BSON( "title" << 10 ) );

            FTSSpec spec( FTSSpec::fixSpec( user ) );

            TermFrequencyMap m;
            spec.scoreDocument( BSON( "title" << "cat sat run" ), &m );
            ASSERT_EQUALS( 3U, m.size() );
            ASSERT_EQUALS( m["cat"], m["sat"] );
            ASSERT_EQUALS( m["cat"], m["run"] );
            ASSERT( m["cat"] > 0 );
        }

        TEST( FTSSpec, ScoreMultipleField1 ) {
            BSONObj user = BSON( "key" << BSON( "title" << "fts" <<
                                                "text" << "fts" ) <<
                                 "weights" << BSON( "title" << 10 ) );

            FTSSpec spec( FTSSpec::fixSpec( user ) );

            TermFrequencyMap m;
            spec.scoreDocument( BSON( "title" << "cat sat run"
                                      << "text" << "cat book" ),
                                &m );

            ASSERT_EQUALS( 4U, m.size() );
            ASSERT_EQUALS( m["sat"], m["run"] );
            ASSERT( m["sat"] > 0 );

            ASSERT( m["cat"] > m["sat"] );
            ASSERT( m["cat"] > m["book"] );
            ASSERT( m["book"] > 0 );
            ASSERT( m["book"] < m["sat"] );
        }


        TEST( FTSSpec, ScoreRepeatWord ) {
            BSONObj user = BSON( "key" << BSON( "title" << "fts" <<
                                                "text" << "fts" ) <<
                                 "weights" << BSON( "title" << 10 ) );

            FTSSpec spec( FTSSpec::fixSpec( user ) );

            TermFrequencyMap m;
            spec.scoreDocument( BSON( "title" << "cat sat sat run run run" ), &m );
            ASSERT_EQUALS( 3U, m.size() );
            ASSERT( m["cat"] > 0 );
            ASSERT( m["sat"] > m["cat"] );
            ASSERT( m["run"] > m["sat"] );

        }

        TEST( FTSSpec, Extra1 ) {
            BSONObj user = BSON( "key" << BSON( "data" << "fts" ) );
            FTSSpec spec( FTSSpec::fixSpec( user ) );
            ASSERT_EQUALS( 0U, spec.numExtraBefore() );
            ASSERT_EQUALS( 0U, spec.numExtraAfter() );
        }

        TEST( FTSSpec, Extra2 ) {
            BSONObj user = BSON( "key" << BSON( "data" << "fts" << "x" << 1 ) );
            FTSSpec spec( FTSSpec::fixSpec( user ) );
            ASSERT_EQUALS( 0U, spec.numExtraBefore() );
            ASSERT_EQUALS( 1U, spec.numExtraAfter() );
            ASSERT_EQUALS( StringData("x"), spec.extraAfter(0) );
        }

        TEST( FTSSpec, Extra3 ) {
            BSONObj user = BSON( "key" << BSON( "x" << 1 << "data" << "fts" ) );
            BSONObj fixed = FTSSpec::fixSpec( user );

            ASSERT_EQUALS( BSON( "x" << 1 <<
                                 "_fts" << "text" <<
                                 "_ftsx" << 1 ),
                           fixed["key"].Obj() );
            ASSERT_EQUALS( BSON( "data" << 1 ),
                           fixed["weights"].Obj() );

            BSONObj fixed2 = FTSSpec::fixSpec( fixed );
            ASSERT_EQUALS( fixed, fixed2 );

            FTSSpec spec( fixed );
            ASSERT_EQUALS( 1U, spec.numExtraBefore() );
            ASSERT_EQUALS( StringData("x"), spec.extraBefore(0) );
            ASSERT_EQUALS( 0U, spec.numExtraAfter() );

            BSONObj prefix;

            ASSERT( spec.getIndexPrefix( BSON( "x" << 2 ), &prefix ).isOK() );
            ASSERT_EQUALS( BSON( "x" << 2 ), prefix );

            ASSERT( spec.getIndexPrefix( BSON( "x" << 3 << "y" << 4 ), &prefix ).isOK() );
            ASSERT_EQUALS( BSON( "x" << 3 ), prefix );

            ASSERT( !spec.getIndexPrefix( BSON( "x" << BSON( "$gt" << 5 ) ), &prefix ).isOK() );
            ASSERT( !spec.getIndexPrefix( BSON( "y" << 4 ), &prefix ).isOK() );
            ASSERT( !spec.getIndexPrefix( BSONObj(), &prefix ).isOK() );
        }

    }
}
