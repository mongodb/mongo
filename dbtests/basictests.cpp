// basictests.cpp : basic unit tests
//

/**
 *    Copyright (C) 2009 10gen Inc.
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

#include "../stdafx.h"

#include "dbtests.h"

namespace BasicTests {

    class Rarely {
    public:
        void run() {
            int first = 0;
            int second = 0;
            int third = 0;
            for( int i = 0; i < 128; ++i ) {
                incRarely( first );
                incRarely2( second );
                ONCE ++third;
            }
            ASSERT_EQUALS( 1, first );
            ASSERT_EQUALS( 1, second );
            ASSERT_EQUALS( 1, third );
        }
    private:
        void incRarely( int &c ) {
            RARELY ++c;
        }
        void incRarely2( int &c ) {
            RARELY ++c;
        }
    };
    
    class All : public Suite {
    public:
        All() : Suite( "basic" ){
        }
        
        void setupTests(){
            add< Rarely >();
        }
    } myall;
    
} // namespace BasicTests

