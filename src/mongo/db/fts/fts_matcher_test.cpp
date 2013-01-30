// fts_matcher_test.cpp

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

#include "mongo/db/fts/fts_matcher.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
    namespace fts {

        TEST( FTSMatcher, NegWild1 ) {
            FTSQuery q;
            q.parse( "foo -bar", "english" );
            FTSMatcher m( q,
                          FTSSpec( FTSSpec::fixSpec( BSON( "key" << BSON( "$**" << "fts" ) ) ) ) );

            ASSERT( m.hasNegativeTerm( BSON( "x" << BSON( "y" << "bar" ) ) ) );
            ASSERT( m.hasNegativeTerm( BSON( "x" << BSON( "y" << "bar" ) ) ) );
        }

        TEST( FTSMatcher, Phrase1 ) {
            FTSQuery q;
            q.parse( "foo \"table top\"", "english" );
            FTSMatcher m( q,
                          FTSSpec( FTSSpec::fixSpec( BSON( "key" << BSON( "$**" << "fts" ) ) ) ) );
            
            ASSERT( m.phraseMatch( "table top", BSON( "x" << "table top" ) ) );
            ASSERT( m.phraseMatch( "table top", BSON( "x" << " asd table top asd" ) ) );
            ASSERT( !m.phraseMatch( "table top", BSON( "x" << "tablz top" ) ) );
            ASSERT( !m.phraseMatch( "table top", BSON( "x" << " asd tablz top asd" ) ) );

            ASSERT( m.phrasesMatch( BSON( "x" << "table top" ) ) );
            ASSERT( !m.phrasesMatch( BSON( "x" << "table a top" ) ) );

        }

        TEST( FTSMatcher, Phrase2 ) {
            FTSQuery q;
            q.parse( "foo \"table top\"", "english" );
            FTSMatcher m( q,
                          FTSSpec( FTSSpec::fixSpec( BSON( "key" << BSON( "x" << "fts" ) ) ) ) );
            ASSERT( m.phraseMatch( "table top",
                                   BSON( "x" << BSON_ARRAY( "table top" ) ) ) );
        }

    }
}
