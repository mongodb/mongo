// socktests.cpp : sock.{h,cpp} unit tests.
//

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
 */

#include "mongo/pch.h"

#include "mongo/dbtests/dbtests.h"
#include "mongo/util/net/sock.h"

namespace SockTests {

    class HostByName {
    public:
        void run() {
            ASSERT_EQUALS( "127.0.0.1", hostbyname( "localhost" ) );
            ASSERT_EQUALS( "127.0.0.1", hostbyname( "127.0.0.1" ) );
            // ASSERT_EQUALS( "::1", hostbyname( "::1" ) ); // IPv6 disabled at runtime by default.

            HostAndPort h("asdfasdfasdf_no_such_host");
            // this fails uncomment when fixed.
            ASSERT( !h.isSelf() );
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "sock" ) {}
        void setupTests() {
            add< HostByName >();
        }
    } myall;

} // namespace SockTests

