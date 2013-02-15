// server_parameters_test.cpp

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

#include "mongo/db/server_parameters.h"

namespace mongo {

    TEST( ServerParameters, Simple1 ) {
        int f = 5;
        ExportedServerParameter<int> ff( NULL, "ff", &f, true, true );
        ASSERT_EQUALS( "ff" , ff.name() );
        ASSERT_EQUALS( 5, ff.get() );

        ff.set( 6 );
        ASSERT_EQUALS( 6, ff.get() );
        ASSERT_EQUALS( 6, f );

        ff.set( BSON( "x" << 7 ).firstElement() );
        ASSERT_EQUALS( 7, ff.get() );
        ASSERT_EQUALS( 7, f );

        ff.setFromString( "8" );
        ASSERT_EQUALS( 8, ff.get() );
        ASSERT_EQUALS( 8, f );

    }

    TEST( ServerParameters, Vector1 ) {
        vector<string> v;

        ExportedServerParameter< vector<string> > vv( NULL, "vv", &v, true, true );

        BSONObj x = BSON( "x" << BSON_ARRAY( "a" << "b" << "c" ) );
        vv.set( x.firstElement() );

        ASSERT_EQUALS( 3U, v.size() );
        ASSERT_EQUALS( "a", v[0] );
        ASSERT_EQUALS( "b", v[1] );
        ASSERT_EQUALS( "c", v[2] );

        BSONObjBuilder b;
        vv.append( b, vv.name() );
        BSONObj y = b.obj();
        ASSERT( x.firstElement().woCompare( y.firstElement(), false ) == 0 );


        vv.setFromString( "d,e" );
        ASSERT_EQUALS( 2U, v.size() );
        ASSERT_EQUALS( "d", v[0] );
        ASSERT_EQUALS( "e", v[1] );
    }

}
