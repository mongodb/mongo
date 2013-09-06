// shard_test.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/s/shard.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    // Note: these are all crutch and hopefully will eventually go away
    CmdLine cmdLine;

    bool inShutdown() {
        return false;
    }

    DBClientBase *createDirectClient() { return NULL; }

    void dbexit(ExitCode rc, const char *why){
        ::_exit(-1);
    }

    bool haveLocalShardingInfo(const string& ns) {
        return false;
    }

    // -----------------------------------

    TEST( Shard, EqualityRs ) {
        Shard a( "foo", "bar/a,b" );
        Shard b( "foo", "bar/a,b" );
        ASSERT_EQUALS( a, b );

        b = Shard( "foo", "bar/b,a" );
        ASSERT_EQUALS( a, b );
    }

    TEST( Shard, EqualitySingle ) {
        ASSERT_EQUALS( Shard( "foo", "b.foo.com:123"), Shard( "foo", "b.foo.com:123") );
        ASSERT_NOT_EQUALS( Shard( "foo", "b.foo.com:123"), Shard( "foo", "a.foo.com:123") );
        ASSERT_NOT_EQUALS( Shard( "foo", "b.foo.com:123"), Shard( "foo", "b.foo.com:124") );
        ASSERT_NOT_EQUALS( Shard( "foo", "b.foo.com:123"), Shard( "foa", "b.foo.com:123") );
    }

    TEST( Shard, EqualitySync ) {
        ConnectionString cs( ConnectionString::SYNC, "a,b,c" );
        ASSERT( cs.sameLogicalEndpoint( ConnectionString( ConnectionString::SYNC, "a,b,c" ) ) );
        ASSERT( cs.sameLogicalEndpoint( ConnectionString( ConnectionString::SYNC, "c,b,a" ) ) );
        ASSERT( cs.sameLogicalEndpoint( ConnectionString( ConnectionString::SYNC, "c,a,b" ) ) );

        ASSERT( ! cs.sameLogicalEndpoint( ConnectionString( ConnectionString::SYNC, "d,a,b" ) ) );
    }
}
