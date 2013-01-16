// tokenizer_test.cpp

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

#include "mongo/db/fts/tokenizer.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
    namespace fts {

        TEST( Tokenizer, Empty1 ) {
            Tokenizer i( "english", "" );
            ASSERT( !i.more() );
        }

        TEST( Tokenizer, Basic1 ) {
            Tokenizer i( "english", "blue red green" );

            ASSERT( i.more() );
            ASSERT_EQUALS( i.next().data.toString(), "blue" );

            ASSERT( i.more() );
            ASSERT_EQUALS( i.next().data.toString(), "red" );

            ASSERT( i.more() );
            ASSERT_EQUALS( i.next().data.toString(), "green" );

            ASSERT( !i.more() );
        }

        TEST( Tokenizer, Basic2 ) {
            Tokenizer i( "english", "blue-red" );

            Token a = i.next();
            Token b = i.next();
            Token c = i.next();
            Token d = i.next();

            ASSERT_EQUALS( Token::TEXT, a.type );
            ASSERT_EQUALS( Token::DELIMITER, b.type );
            ASSERT_EQUALS( Token::TEXT, c.type );
            ASSERT_EQUALS( Token::INVALID, d.type );

            ASSERT_EQUALS( "blue", a.data.toString() );
            ASSERT_EQUALS( "-", b.data.toString() );
            ASSERT_EQUALS( "red", c.data.toString() );

            ASSERT( a.previousWhiteSpace );
            ASSERT( !b.previousWhiteSpace );
            ASSERT( !c.previousWhiteSpace );
        }

        TEST( Tokenizer, Basic3 ) {
            Tokenizer i( "english", "blue -red" );

            Token a = i.next();
            Token b = i.next();
            Token c = i.next();
            Token d = i.next();

            ASSERT_EQUALS( Token::TEXT, a.type );
            ASSERT_EQUALS( Token::DELIMITER, b.type );
            ASSERT_EQUALS( Token::TEXT, c.type );
            ASSERT_EQUALS( Token::INVALID, d.type );

            ASSERT_EQUALS( "blue", a.data.toString() );
            ASSERT_EQUALS( "-", b.data.toString() );
            ASSERT_EQUALS( "red", c.data.toString() );

            ASSERT( a.previousWhiteSpace );
            ASSERT( b.previousWhiteSpace );
            ASSERT( !c.previousWhiteSpace );


            ASSERT_EQUALS( 0U, a.offset );
            ASSERT_EQUALS( 5U, b.offset );
            ASSERT_EQUALS( 6U, c.offset );
        }

        TEST( Tokenizer, Quote1English ) {
            Tokenizer i( "english", "eliot's car" );

            Token a = i.next();
            Token b = i.next();

            ASSERT_EQUALS( "eliot's", a.data.toString() );
            ASSERT_EQUALS( "car", b.data.toString() );
        }

        TEST( Tokenizer, Quote1French ) {
            Tokenizer i( "french", "eliot's car" );

            Token a = i.next();
            Token b = i.next();
            Token c = i.next();

            ASSERT_EQUALS( "eliot", a.data.toString() );
            ASSERT_EQUALS( "s", b.data.toString() );
            ASSERT_EQUALS( "car", c.data.toString() );
        }

    }
}


