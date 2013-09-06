// fts_index_format_test.cpp

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


#include "mongo/pch.h"

#include "mongo/db/fts/fts_index_format.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    namespace fts {

        TEST( FTSIndexFormat, Simple1 ) {
            FTSSpec spec( FTSSpec::fixSpec( BSON( "key" << BSON( "data" << "text" ) ) ) );
            BSONObjSet keys;
            FTSIndexFormat::getKeys( spec, BSON( "data" << "cat sat" ), &keys );

            ASSERT_EQUALS( 2U, keys.size() );
            for ( BSONObjSet::const_iterator i = keys.begin(); i != keys.end(); ++i ) {
                BSONObj key = *i;
                ASSERT_EQUALS( 2, key.nFields() );
                ASSERT_EQUALS( String, key.firstElement().type() );
            }
        }

        TEST( FTSIndexFormat, ExtraBack1 ) {
            FTSSpec spec( FTSSpec::fixSpec( BSON( "key" << BSON( "data" << "text" <<
                                                                 "x" << 1 ) ) ) );
            BSONObjSet keys;
            FTSIndexFormat::getKeys( spec, BSON( "data" << "cat" << "x" << 5 ), &keys );

            ASSERT_EQUALS( 1U, keys.size() );
            BSONObj key = *(keys.begin());
            ASSERT_EQUALS( 3, key.nFields() );
            BSONObjIterator i( key );
            ASSERT_EQUALS( StringData("cat"), i.next().valuestr() );
            ASSERT( i.next().numberDouble() > 0 );
            ASSERT_EQUALS( 5, i.next().numberInt() );
        }

        /*
        TEST( FTSIndexFormat, ExtraBackArray1 ) {
            FTSSpec spec( FTSSpec::fixSpec( BSON( "key" << BSON( "data" << "text" <<
                                                                 "x.y" << 1 ) ) ) );
            BSONObjSet keys;
            FTSIndexFormat::getKeys( spec,
                                     BSON( "data" << "cat" <<
                                           "x" << BSON_ARRAY( BSON( "y" << 1 ) <<
                                                              BSON( "y" << 2 ) ) ),
                                     &keys );

            ASSERT_EQUALS( 1U, keys.size() );
            BSONObj key = *(keys.begin());
            log() << "e: " << key << endl;
            ASSERT_EQUALS( 3, key.nFields() );
            BSONObjIterator i( key );
            ASSERT_EQUALS( StringData("cat"), i.next().valuestr() );
            ASSERT( i.next().numberDouble() > 0 );
            ASSERT_EQUALS( 5, i.next().numberInt() );
        }
        */

        TEST( FTSIndexFormat, ExtraFront1 ) {
            FTSSpec spec( FTSSpec::fixSpec( BSON( "key" << BSON( "x" << 1 <<
                                                                 "data" << "text" ) ) ) );
            BSONObjSet keys;
            FTSIndexFormat::getKeys( spec, BSON( "data" << "cat" << "x" << 5 ), &keys );

            ASSERT_EQUALS( 1U, keys.size() );
            BSONObj key = *(keys.begin());
            ASSERT_EQUALS( 3, key.nFields() );
            BSONObjIterator i( key );
            ASSERT_EQUALS( 5, i.next().numberInt() );
            ASSERT_EQUALS( StringData("cat"), i.next().valuestr() );
            ASSERT( i.next().numberDouble() > 0 );
        }

        TEST( FTSIndexFormat, StopWords1 ) {
            FTSSpec spec( FTSSpec::fixSpec( BSON( "key" << BSON( "data" << "text" ) ) ) );

            BSONObjSet keys1;
            FTSIndexFormat::getKeys( spec, BSON( "data" << "computer" ), &keys1 );
            ASSERT_EQUALS( 1U, keys1.size() );

            BSONObjSet keys2;
            FTSIndexFormat::getKeys( spec, BSON( "data" << "any computer" ), &keys2 );
            ASSERT_EQUALS( 1U, keys2.size() );
        }


    }
}
