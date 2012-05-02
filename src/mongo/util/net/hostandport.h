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
#include "../../db/cmdline.h"
#include "../mongoutils/str.h"

namespace mongo {

    using namespace mongoutils;

    void dynHostResolve(string& name, int& port);
    string dynHostMyName();

    /** helper for manipulating host:port connection endpoints.
      */
    struct HostAndPort {
        HostAndPort() : _port(-1) { }

        /** From a string hostname[:portnumber] or a #dynname
            Throws user assertion if bad config string or bad port #.
        */
        HostAndPort(string s);

        /** @param p port number. -1 is ok to use default. */
        HostAndPort(string h, int p /*= -1*/) : _host(h), _port(p) { 
            verify( !str::startsWith(h, '#') );
        }

        HostAndPort(const SockAddr& sock ) : _host( sock.getAddr() ) , _port( sock.getPort() ) { }

        static HostAndPort me() { return HostAndPort("localhost", cmdLine.port); }

        /* uses real hostname instead of localhost */
        static HostAndPort Me();

        bool operator<(const HostAndPort& r) const {
            string h = host();
            string rh = r.host();
            if( h < rh )
                return true;
            if( h == rh )
                return port() < r.port();
            return false;
        }

        bool operator==(const HostAndPort& r) const { 
            return host() == r.host() && port() == r.port(); 
        }

        bool operator!=(const HostAndPort& r) const { return !(*this == r); }

        /* returns true if the host/port combo identifies this process instance. */
        bool isSelf() const; // defined in isself.cpp

        bool isLocalHost() const;

        /**
         * @param includePort host:port if true, host otherwise
         */
        string toString( bool includePort=true ) const;

        // get the logical name if using a #dynhostname instead of resolving to current actual name
        string dynString() const;
        string toStringLong() const;

        operator string() const { return toString(); }

        bool empty() const { 
            return _dynName.empty() && _host.empty() && _port < 0;
        }
        string host() const { 
            if( !dyn() )
                return _host; 
            string h = _dynName;
            int p;
            dynHostResolve(h, p);
            return h;
        }
        int port() const { 
            int p = -2;
            if( dyn() ) {
                string h = _dynName;
                dynHostResolve(h,p);
            }
            else {
                p = _port;
            }
            return p >= 0 ? p : CmdLine::DefaultDBPort; 
        }
        bool hasPort() const { 
            int p = -2;
            if( dyn() ) {
                string h = _dynName;
                dynHostResolve(h,p);
            }
            else {
                p = _port;
            }
            return p >= 0;
        }
        void setPort( int port ) { 
            if( dyn() ) {
                log() << "INFO skipping setPort() HostAndPort dyn()=true" << endl;
                return;
            }
            _port = port; 
        }

    private:
        bool dyn() const { return !_dynName.empty(); }
        void init(const char *);
        // invariant (except full obj assignment):
        string _dynName; // when this is set, _host and _port aren't used, rather, we look up the dyn info every time.
        string _host;
        int _port; // -1 indicates unspecified
    };

    inline HostAndPort HostAndPort::Me() {
        {
            string s = dynHostMyName();
            if( !s.empty() ) 
                return HostAndPort(s);
        }

        const char* ips = cmdLine.bind_ip.c_str();
        while(*ips) {
            string ip;
            const char * comma = strchr(ips, ',');
            if (comma) {
                ip = string(ips, comma - ips);
                ips = comma + 1;
            }
            else {
                ip = string(ips);
                ips = "";
            }
            HostAndPort h = HostAndPort(ip, cmdLine.port);
            if (!h.isLocalHost()) {
                return h;
            }
        }

        string h = getHostName();
        verify( !h.empty() );
        verify( h != "localhost" );
        return HostAndPort(h, cmdLine.port);
    }

    inline string HostAndPort::dynString() const {
        return dyn() ? _dynName : toString();
    }

    inline string HostAndPort::toStringLong() const {
        return _dynName + ':' + toString();
    }

    inline string HostAndPort::toString( bool includePort ) const {
        string h = host();
        int p = port();

        if ( ! includePort )
            return h;

        stringstream ss;
        ss << h;
        if ( p != -1 ) {
            ss << ':';
#if defined(_DEBUG)
            if( p >= 44000 && p < 44100 ) {
                log() << "warning: special debug port 44xxx used" << endl;
                ss << p+1;
            }
            else
                ss << p;
#else
            ss << p;
#endif
        }
        return ss.str();
    }

    inline bool HostAndPort::isLocalHost() const {
        string _host = host();
        return (  _host == "localhost"
               || startsWith(_host.c_str(), "127.")
               || _host == "::1"
               || _host == "anonymous unix socket"
               || _host.c_str()[0] == '/' // unix socket
               );
    }

    inline void HostAndPort::init(const char *p) {
        massert(13110, "HostAndPort: host is empty", *p);
        verify( *p != '#' );
        verify( _dynName.empty() );
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

    inline HostAndPort::HostAndPort(string s) {
        const char *p = s.c_str();
        if( *p == '#' ) {
            _dynName = s;
            _port = -2;
            _host = "invalid_hostname_dyn_in_use";
        }
        else {
            init(p);
        }
    }

}
