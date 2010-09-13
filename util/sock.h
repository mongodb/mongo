// @file sock.h

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

#include "../pch.h"

#include <stdio.h>
#include <sstream>
#include "goodies.h"
#include "../db/jsobj.h"

namespace mongo {

    const int SOCK_FAMILY_UNKNOWN_ERROR=13078;
    string getAddrInfoStrError(int code);

#if defined(_WIN32)

    typedef short sa_family_t;
    typedef int socklen_t;
    inline int getLastError() { return WSAGetLastError(); }
    inline void disableNagle(int sock) {
        int x = 1;
        if ( setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &x, sizeof(x)) )
            out() << "ERROR: disableNagle failed" << endl;
        if ( setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *) &x, sizeof(x)) )
            out() << "ERROR: SO_KEEPALIVE failed" << endl;
    }
    inline void prebindOptions( int sock ) { }

    // This won't actually be used on windows
    struct sockaddr_un {
        short sun_family;
        char sun_path[108]; // length from unix header
    };

#else

} // namespace mongo

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#ifdef __openbsd__
# include <sys/uio.h>
#endif

#ifndef AI_ADDRCONFIG
# define AI_ADDRCONFIG 0
#endif

namespace mongo {

    inline void closesocket(int s) {
        close(s);
    }
    const int INVALID_SOCKET = -1;
    typedef int SOCKET;

    inline void disableNagle(int sock) {
        int x = 1;

#ifdef SOL_TCP
        int level = SOL_TCP;
#else
        int level = SOL_SOCKET;
#endif

        if ( setsockopt(sock, level, TCP_NODELAY, (char *) &x, sizeof(x)) )
            log() << "ERROR: disableNagle failed: " << errnoWithDescription() << endl;

#ifdef SO_KEEPALIVE
        if ( setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *) &x, sizeof(x)) )
            log() << "ERROR: SO_KEEPALIVE failed: " << errnoWithDescription() << endl;
#endif

    }
    inline void prebindOptions( int sock ) {
        DEV log() << "doing prebind option" << endl;
        int x = 1;
        if ( setsockopt( sock , SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x)) < 0 )
            out() << "Failed to set socket opt, SO_REUSEADDR" << endl;
    }


#endif

    inline string makeUnixSockPath(int port){
        return "/tmp/mongodb-" + BSONObjBuilder::numStr(port) + ".sock";
    }

    inline void setSockTimeouts(int sock, double secs) {
        struct timeval tv;
        tv.tv_sec = (int)secs;
        tv.tv_usec = (int)((long long)(secs*1000*1000) % (1000*1000));
        bool report = logLevel > 3; // solaris doesn't provide these
        DEV report = true;
        bool ok = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &tv, sizeof(tv) ) == 0;
        if( report && !ok ) log() << "unabled to set SO_RCVTIMEO" << endl;
        ok = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *) &tv, sizeof(tv) ) == 0;
        DEV if( report && !ok ) log() << "unabled to set SO_RCVTIMEO" << endl;
    }

    // If an ip address is passed in, just return that.  If a hostname is passed
    // in, look up its ip and return that.  Returns "" on failure.
    string hostbyname(const char *hostname);

    void enableIPv6(bool state=true);
    bool IPv6Enabled();

    struct SockAddr {
        SockAddr() {
            addressSize = sizeof(sa);
            memset(&sa, 0, sizeof(sa));
            sa.ss_family = AF_UNSPEC;
        }
        SockAddr(int sourcePort); /* listener side */
        SockAddr(const char *ip, int port); /* EndPoint (remote) side, or if you want to specify which interface locally */

        template <typename T>
        T& as() { return *(T*)(&sa); }
        template <typename T>
        const T& as() const { return *(const T*)(&sa); }

        string toString(bool includePort=true) const{
            string out = getAddr();
            if (includePort && getType() != AF_UNIX && getType() != AF_UNSPEC)
                out += ':' + BSONObjBuilder::numStr(getPort());
            return out;
        }

        // returns one of AF_INET, AF_INET6, or AF_UNIX
        sa_family_t getType() const {
            return sa.ss_family;
        }

        unsigned getPort() const {
            switch (getType()){
                case AF_INET:  return ntohs(as<sockaddr_in>().sin_port);
                case AF_INET6: return ntohs(as<sockaddr_in6>().sin6_port);
                case AF_UNIX: return 0;
                case AF_UNSPEC: return 0;
                default: massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false); return 0;
            }
        }

        string getAddr() const {
            switch (getType()){
                case AF_INET:
                case AF_INET6: {
                    const int buflen=128;
                    char buffer[buflen];
                    int ret = getnameinfo(raw(), addressSize, buffer, buflen, NULL, 0, NI_NUMERICHOST);
                    massert(13082, getAddrInfoStrError(ret), ret == 0);
                    return buffer;
                }

                case AF_UNIX:  return (addressSize > 2 ? as<sockaddr_un>().sun_path : "anonymous unix socket");
                case AF_UNSPEC: return "(NONE)";
                default: massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false); return "";
            }
        }

        bool isLocalHost() const;
        
        bool operator==(const SockAddr& r) const {
            if (getType() != r.getType())
                return false;

            if (getPort() != r.getPort())
                return false;

            switch (getType()){
                case AF_INET:  return as<sockaddr_in>().sin_addr.s_addr == r.as<sockaddr_in>().sin_addr.s_addr;
                case AF_INET6: return memcmp(as<sockaddr_in6>().sin6_addr.s6_addr, r.as<sockaddr_in6>().sin6_addr.s6_addr, sizeof(in6_addr)) == 0;
                case AF_UNIX:  return strcmp(as<sockaddr_un>().sun_path, r.as<sockaddr_un>().sun_path) == 0;
                case AF_UNSPEC: return true; // assume all unspecified addresses are the same
                default: massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false);
            }
        }
        bool operator!=(const SockAddr& r) const {
            return !(*this == r);
        }
        bool operator<(const SockAddr& r) const {
            if (getType() < r.getType())
                return true;
            else if (getType() > r.getType())
                return false;

            if (getPort() < r.getPort())
                return true;
            else if (getPort() > r.getPort())
                return false;

            switch (getType()){
                case AF_INET:  return as<sockaddr_in>().sin_addr.s_addr < r.as<sockaddr_in>().sin_addr.s_addr;
                case AF_INET6: return memcmp(as<sockaddr_in6>().sin6_addr.s6_addr, r.as<sockaddr_in6>().sin6_addr.s6_addr, sizeof(in6_addr)) < 0;
                case AF_UNIX:  return strcmp(as<sockaddr_un>().sun_path, r.as<sockaddr_un>().sun_path) < 0;
                case AF_UNSPEC: return false;
                default: massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false);
            }
        }

        const sockaddr* raw() const {return (sockaddr*)&sa;}
        sockaddr* raw() {return (sockaddr*)&sa;}

        socklen_t addressSize;
        private:
        struct sockaddr_storage sa;
    };

    extern SockAddr unknownAddress; // ( "0.0.0.0", 0 )

    const int MaxMTU = 16384;

    inline string getHostName() {
        char buf[256];
        int ec = gethostname(buf, 127);
        if ( ec || *buf == 0 ) {
            log() << "can't get this server's hostname " << errnoWithDescription() << endl;
            return "";
        }
        return buf;
    }

    string getHostNameCached();

    class ListeningSockets {
    public:
        ListeningSockets() : _mutex("ListeningSockets"), _sockets( new set<int>() ) { }
        void add( int sock ){
            scoped_lock lk( _mutex );
            _sockets->insert( sock );
        }
        void remove( int sock ){
            scoped_lock lk( _mutex );
            _sockets->erase( sock );
        }
        void closeAll(){
            set<int>* s;
            {
                scoped_lock lk( _mutex );
                s = _sockets;
                _sockets = new set<int>();
            }
            for ( set<int>::iterator i=s->begin(); i!=s->end(); i++ ) {
                int sock = *i;
                log() << "closing listening socket: " << sock << endl;
                closesocket( sock );
            }            
        }
        static ListeningSockets* get();
    private:
        mongo::mutex _mutex;
        set<int>* _sockets;
        static ListeningSockets* _instance;
    };

} // namespace mongo
