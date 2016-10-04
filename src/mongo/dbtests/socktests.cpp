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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/isself.h"
#include "mongo/dbtests/dbtests.h"

namespace SockTests {

class HostByName {
public:
    void run() {
        ASSERT_EQUALS("127.0.0.1", hostbyname("localhost"));
        ASSERT_EQUALS("127.0.0.1", hostbyname("127.0.0.1"));
        // ASSERT_EQUALS( "::1", hostbyname( "::1" ) ); // IPv6 disabled at runtime by default.

        HostAndPort h("asdfasdfasdf_no_such_host");
        ASSERT_EQUALS("", hostbyname("asdfasdfasdf_no_such_host"));
    }
};

class All : public Suite {
public:
    All() : Suite("sock") {}
    void setupTests() {
        add<HostByName>();
    }
};

SuiteInstance<All> myall;

}  // namespace SockTests
