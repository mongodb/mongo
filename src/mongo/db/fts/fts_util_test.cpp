// fts_util_test.cpp

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

#include "mongo/db/fts/fts_util.h"

namespace mongo {
    namespace fts {

        TEST( BSONElementMap, Simple1 ) {
            BSONElementMap<double> m;

            BSONObj x = BSON( "x" << 5 );
            m[x.firstElement()] = 5;
            ASSERT_EQUALS( 5, m[x.firstElement()] );
        }

    }
}
