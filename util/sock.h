// sock.h

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

#include "../stdafx.h"

#include <stdio.h>
#include <sstream>
#include "goodies.h"
#include "../db/jsobj.h"

#define SOCK_FAMILY_UNKNOWN_ERROR 13078

namespace mongo {

#if defined(_WIN32)

    typedef short sa_family_t;
    typedef int socklen_t;
    inline int getLastError() {
        return WSAGetLastError();
    }
    inline void disableNagle(int sock) {
        int x = 1;
        if ( setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &x, sizeof(x)) )
            out() << "ERROR: disableNagle failed" << endl;
    }
    inline void prebindOptions( int sock ) {
    }

    // This won't actually be used on windows
    struct sockaddr_un {
        short sun_family;
        char sun_path[108]; // length from unix header
    };

    // Windows doesn't const-qualify src for some reason
    const char* inet_ntop(int af, const void* src, char* dst, socklen_t size){
        return ::inet_ntop(af, const_cast<void*>(src),dst,size);
    }
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
            log() << "ERROR: disableNagle failed" << endl;

    }
    inline void prebindOptions( int sock ) {
        DEV log() << "doing prebind option" << endl;
        int x = 1;
        if ( setsockopt( sock , SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x)) < 0 )
            out() << "Failed to set socket opt, SO_REUSEADDR" << endl;
    }


#endif

    inline void setSockReceiveTimeout(int sock, int secs) {
// todo - finish - works?
        struct timeval tv;
        tv.tv_sec = 0;//secs;
        tv.tv_usec = 1000;
        int rc = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &tv, sizeof(tv));
        if ( rc ) {
            out() << "ERROR: setsockopt RCVTIMEO failed rc:" << rc << " " << OUTPUT_ERRNO << " secs:" << secs << " sock:" << sock << endl;
        }
    }

    // If an ip address is passed in, just return that.  If a hostname is passed
    // in, look up its ip and return that.  Returns "" on failure.
    string hostbyname(const char *hostname);

    struct SockAddr {
        SockAddr() {
            addressSize = sizeof(sockaddr_in);
            memset(&sa, 0, sizeof(sa));
        }
        SockAddr(int sourcePort); /* listener side */
        SockAddr(const char *ip, int port); /* EndPoint (remote) side, or if you want to specify which interface locally */

        template <typename T>
        T& as() { return *(T*)(&sa); }
        template <typename T>
        const T& as() const { return *(const T*)(&sa); }

        string toString(bool includePort=true) const{
            string out = getAddr();
            if (includePort && getType() != AF_UNIX)
                out += ':' + BSONObjBuilder::numStr(getPort());
            return out;
        }

        operator string() const{
            return toString();
        }

        // returns one of AF_INET, AF_INET6, or AF_UNIX
        sa_family_t getType() const {
            return sa.ss_family;
        }

        unsigned getPort() const {
            switch (getType()){
                case AF_INET:  return as<sockaddr_in>().sin_port;
                case AF_INET6: return as<sockaddr_in6>().sin6_port;
                case AF_UNIX: return 0;
                default: massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false); return 0;
            }
        }

        string getAddr() const {
            const int buflen=128;
            char buffer[buflen];

            switch (getType()){
                case AF_INET:  return inet_ntop(getType(), &as<sockaddr_in>().sin_addr, buffer, addressSize);
                case AF_INET6: return inet_ntop(getType(), &as<sockaddr_in6>().sin6_addr, buffer, addressSize);
                case AF_UNIX:  return as<sockaddr_un>().sun_path;
                default: massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false); return "";
            }
        }

        bool isLocalHost() const { return inet_addr( "127.0.0.1" ) == as<sockaddr_in>().sin_addr.s_addr; }
        
        bool operator==(const SockAddr& r) const {
            if (getType() != r.getType())
                return false;

            if (getPort() != r.getPort())
                return false;

            switch (getType()){
                case AF_INET:  return as<sockaddr_in>().sin_addr.s_addr == r.as<sockaddr_in>().sin_addr.s_addr;
                case AF_INET6: return memcmp(as<sockaddr_in6>().sin6_addr.s6_addr, r.as<sockaddr_in6>().sin6_addr.s6_addr, sizeof(in6_addr)) == 0;
                case AF_UNIX:  return strcmp(as<sockaddr_un>().sun_path, r.as<sockaddr_un>().sun_path) == 0;
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
                default: massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false);
            }
        }

        const sockaddr* raw() const {return (sockaddr*)&sa;}
        sockaddr* raw() {return (sockaddr*)&sa;}

        socklen_t addressSize;
        private:
        struct sockaddr_storage sa;
    };

    const int MaxMTU = 16384;

    class UDPConnection {
    public:
        UDPConnection() {
            sock = 0;
        }
        ~UDPConnection() {
            if ( sock ) {
                closesocket(sock);
                sock = 0;
            }
        }
        bool init(const SockAddr& myAddr);
        int recvfrom(char *buf, int len, SockAddr& sender);
        int sendto(char *buf, int len, const SockAddr& EndPoint);
        int mtu(const SockAddr& sa) {
            return sa.isLocalHost() ? 16384 : 1480;
        }

        SOCKET sock;
    };

    inline int UDPConnection::recvfrom(char *buf, int len, SockAddr& sender) {
        return ::recvfrom(sock, buf, len, 0, sender.raw(), &sender.addressSize);
    }

    inline int UDPConnection::sendto(char *buf, int len, const SockAddr& EndPoint) {
        if ( 0 && rand() < (RAND_MAX>>4) ) {
            out() << " NOTSENT ";
            return 0;
        }
        return ::sendto(sock, buf, len, 0, EndPoint.raw(), EndPoint.addressSize);
    }

    inline bool UDPConnection::init(const SockAddr& myAddr) {
        sock = socket(myAddr.getType(), SOCK_DGRAM, IPPROTO_UDP);
        if ( sock == INVALID_SOCKET ) {
            out() << "invalid socket? " << OUTPUT_ERRNO << endl;
            return false;
        }
        if ( ::bind(sock, myAddr.raw(), myAddr.addressSize) != 0 ) {
            out() << "udp init failed" << endl;
            closesocket(sock);
            sock = 0;
            return false;
        }
        socklen_t optLen;
        int rcvbuf;
        if (getsockopt(sock,
                       SOL_SOCKET,
                       SO_RCVBUF,
                       (char*)&rcvbuf,
                       &optLen) != -1)
            out() << "SO_RCVBUF:" << rcvbuf << endl;
        return true;
    }

    inline SockAddr::SockAddr(int sourcePort) {
        memset(as<sockaddr_in>().sin_zero, 0, sizeof(as<sockaddr_in>().sin_zero));
        as<sockaddr_in>().sin_family = AF_INET;
        as<sockaddr_in>().sin_port = htons(sourcePort);
        as<sockaddr_in>().sin_addr.s_addr = htonl(INADDR_ANY);
        addressSize = sizeof(sockaddr_in);
    }

    inline SockAddr::SockAddr(const char * iporhost , int port) {
        if (strchr(iporhost, '/')){
#ifdef _WIN32
            uassert(13080, "no unix socket support on windows", false);
#endif
            uassert(13079, "path to unix socket too long", strlen(iporhost) < sizeof(as<sockaddr_un>().sun_path));
            as<sockaddr_un>().sun_family = AF_UNIX;
            strcpy(as<sockaddr_un>().sun_path, iporhost);
            addressSize = sizeof(sockaddr_un);
        } else {
            string ip = hostbyname( iporhost );
            memset(as<sockaddr_in>().sin_zero, 0, sizeof(as<sockaddr_in>().sin_zero));
            as<sockaddr_in>().sin_family = AF_INET;
            as<sockaddr_in>().sin_port = htons(port);
            as<sockaddr_in>().sin_addr.s_addr = inet_addr(ip.c_str());
            addressSize = sizeof(sockaddr_in);
        }
    }

    inline string getHostName() {
        char buf[256];
        int ec = gethostname(buf, 127);
        if ( ec || *buf == 0 ) {
            log() << "can't get this server's hostname " << OUTPUT_ERRNO << endl;
            return "";
        }
        return buf;
    }

    class ListeningSockets {
    public:
        ListeningSockets() : _sockets( new set<int>() ){
        }
        
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

            for ( set<int>::iterator i=s->begin(); i!=s->end(); i++ ){
                int sock = *i;
                log() << "\t going to close listening socket: " << sock << endl;
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
