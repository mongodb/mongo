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
#include "mongoutils/str.h"
 
namespace mongo { 

    using namespace mongoutils;

    /** helper for manipulating host:port connection endpoints. 
      */
    struct HostAndPort { 
        HostAndPort() : _port(-1) { }

        /** From a string hostname[:portnumber] 
            Throws user assertion if bad config string or bad port #.
            */
        HostAndPort(string s);

        /** @param p port number. -1 is ok to use default. */
        HostAndPort(string h, int p /*= -1*/) : _host(h), _port(p) { }

        HostAndPort(const SockAddr& sock ) 
            : _host( sock.getAddr() ) , _port( sock.getPort() ){
        }

        static HostAndPort me() { 
            return HostAndPort("localhost", cmdLine.port);
        }

        /* uses real hostname instead of localhost */
        static HostAndPort Me();

        bool operator<(const HostAndPort& r) const {
            if( _host < r._host ) 
                return true;
            if( _host == r._host )
                return port() < r.port();
            return false;
        }

        bool operator==(const HostAndPort& r) const {
            return _host == r._host && port() == r.port();
        }

        bool isLocalHost() const;

        // @returns host:port
        string toString() const; 

        operator string() const { return toString(); }

        string host() const { return _host; }

        int port() const { return _port >= 0 ? _port : CmdLine::DefaultDBPort; }
        bool hasPort() const { return _port >= 0; }
        void setPort( int port ) { _port = port; }

    private:
        // invariant (except full obj assignment):
        string _host;
        int _port; // -1 indicates unspecified
    };

    /** returns true if strings seem to be the same hostname.
        "nyc1" and "nyc1.acme.com" are treated as the same.
        in fact "nyc1.foo.com" and "nyc1.acme.com" are treated the same - 
        we oly look up to the first period.
    */
    inline bool sameHostname(const string& a, const string& b) {
        return str::before(a, '.') == str::before(b, '.');
    }

    inline HostAndPort HostAndPort::Me() { 
        string h = getHostName();
        assert( !h.empty() );
        assert( h != "localhost" );
        return HostAndPort(h, cmdLine.port);
    }

    inline string HostAndPort::toString() const {
        stringstream ss;
        ss << _host;
        if ( _port != -1 ){
            ss << ':';
#if defined(_DEBUG)
            if( _port >= 44000 && _port < 44100 ) { 
                log() << "warning: special debug port 44xxx used" << endl;
                ss << _port+1;
            }
            else
                ss << _port;
#else
            ss << _port;
#endif
        }
        return ss.str();
    }

    inline bool HostAndPort::isLocalHost() const { 
        return _host == "localhost" || startsWith(_host.c_str(), "127.") || _host == "::1";
    }

    inline HostAndPort::HostAndPort(string s) {
        const char *p = s.c_str();
        uassert(13110, "HostAndPort: bad config string", *p);
        const char *colon = strrchr(p, ':');
        if( colon ) {
            int port = atoi(colon+1);
            uassert(13095, "HostAndPort: bad port #", port > 0);
            _host = string(p,colon-p);
            _port = port;
        }
        else {
            // no port specified.
            _host = p;
            _port = -1;
        }
    }

}
