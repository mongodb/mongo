// hostandport.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "sock.h"
#include "../db/cmdline.h"

namespace mongo { 

    /** helper for manipulating host:port connection endpoints. 
      */
    struct HostAndPort { 
        HostAndPort() : _port(-1) { }

        HostAndPort(string h, int p = -1) : _host(h), _port(p) { }

        bool operator<(const HostAndPort& r) const { return _host < r._host || (_host==r._host&&_port<r._port); }

        /* returns true if the host/port combo identifies this process instance. */
        bool isSelf() const;

        bool isLocalHost() const;

        string toString();
    private:
        // invariant (except full obj assignment):
        string _host;
        int _port; // -1 indicates unspecified
    };

    /** returns true if strings share a common starting prefix */
    inline bool sameStart(const char *p, const char *q) {
        while( 1 ) {
            if( *p == 0 || *q == 0 )
                return true;
            if( *p != *q )
                break;
            p++; q++;
        }
        return false;
    }

    inline bool HostAndPort::isSelf() const { 
        int p = _port == -1 ? CmdLine::DefaultDBPort : _port;
        if( p != cmdLine.port )
            return false;
        assert( _host != "localhost" && _host != "127.0.0.1" );
        return sameStart(getHostName().c_str(), _host.c_str());
    }

    inline string HostAndPort::toString() {
        stringstream ss;
        ss << _host;
        if( _port != -1 ) ss << ':' << _port;
        return ss.str();
    }

    inline bool HostAndPort::isLocalHost() const { 
        return _host == "localhost" || _host == "127.0.0.1";
    }

}
