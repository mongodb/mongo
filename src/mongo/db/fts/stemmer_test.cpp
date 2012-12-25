// stemmer_test.cpp

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


#include "mongo/unittest/unittest.h"

#include "mongo/db/fts/stemmer.h"

namespace mongo {
    namespace fts {

        TEST( English, Stemmer1 ) {
            Stemmer s( "english" );
            ASSERT_EQUALS( "run", s.stem( "running" ) );
            ASSERT_EQUALS( "Run", s.stem( "Running" ) );
        }


        TEST( English, Caps ) {
            Stemmer s( "porter" );
            ASSERT_EQUALS( "unit", s.stem( "united" ) );
            ASSERT_EQUALS( "Unite", s.stem( "United" ) );
        }


    }
}
