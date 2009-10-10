// connection.h 

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

/* Connection represents a connection to the database (the server-side) and corresponds 
   to an open socket from a client.

   todo: switch to asio...this will fit nicely with that.
*/

#pragma once

#include "lasterror.h"
#include "security.h"

namespace mongo { 

    /* TODO: _ i bet these are not cleaned up on thread exit?  if so fix */

    class Connection { 
    public:
        AuthenticationInfo *ai;

        Connection() { ai = new AuthenticationInfo(); }
        ~Connection() { delete ai; }

        static void initThread();
    };

    extern boost::thread_specific_ptr<Connection> currentConnection;

    inline void Connection::initThread() {
        assert( currentConnection.get() == 0 );
        currentConnection.reset( new Connection() );
    }

};
