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

#include "mongo/client/gridfs.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/assert_util.h"

using mongo::DBDirectClient;
using mongo::GridFS;
using mongo::MsgAssertionException;

namespace {
    DBDirectClient _client;
    
    class SetChunkSizeTest {
    public:
        virtual void run() {
            GridFS grid( _client, "gridtest" );
            grid.setChunkSize( 5 );

            ASSERT_EQUALS( 5U, grid.getChunkSize() );
            ASSERT_THROWS( grid.setChunkSize( 0 ), MsgAssertionException );
            ASSERT_EQUALS( 5U, grid.getChunkSize() );
        }

        virtual ~SetChunkSizeTest() {}
    };

    class All : public Suite {
    public:
        All() : Suite( "gridfs" ) {
        }

        void setupTests() {
            add< SetChunkSizeTest >();
        }
    } myall;
}

