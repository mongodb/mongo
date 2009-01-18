// security.h

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

#pragma once

#include <boost/thread/tss.hpp>

namespace mongo {

    /* for a particular db */
    struct Auth {
        int level;
    };

    class AuthenticationInfo : boost::noncopyable {
        map<string, Auth> m;
    public:
        AuthenticationInfo() { }
        ~AuthenticationInfo() {
        }
    };

    extern boost::thread_specific_ptr<AuthenticationInfo> authInfo;

    typedef unsigned long long nonce;

    struct Security {
        ifstream *devrandom;
        Security();
        nonce getNonce();
    };

    extern Security security;

} // namespace mongo
