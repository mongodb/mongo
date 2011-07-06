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

#include "../../pch.h"

#include <stdio.h>
#include <sstream>
#include "../goodies.h"
#include "../../db/cmdline.h"
#include "../mongoutils/str.h"

#ifndef _WIN32

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#ifdef __openbsd__
# include <sys/uio.h>
#endif

#endif // _WIN32

namespace mongo {

    const int SOCK_FAMILY_UNKNOWN_ERROR=13078;

    void disableNagle(int sock);

#if defined(_WIN32)

    typedef short sa_family_t;
    typedef int socklen_t;

    // This won't actually be used on windows
    struct sockaddr_un {
        short sun_family;
        char sun_path[108]; // length from unix header
    };

#else // _WIN32

    inline void closesocket(int s) { close(s); }
    const int INVALID_SOCKET = -1;
    typedef int SOCKET;

#endif // _WIN32

    inline string makeUnixSockPath(int port) {
        return mongoutils::str::stream() << cmdLine.socket << "/mongodb-" << port << ".sock";
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

    /**
     * wrapped around os representation of network address
     */
    struct SockAddr {
        SockAddr() {
            addressSize = sizeof(sa);
            memset(&sa, 0, sizeof(sa));
            sa.ss_family = AF_UNSPEC;
        }
        SockAddr(int sourcePort); /* listener side */
        SockAddr(const char *ip, int port); /* EndPoint (remote) side, or if you want to specify which interface locally */

        template <typename T> T& as() { return *(T*)(&sa); }
        template <typename T> const T& as() const { return *(const T*)(&sa); }
        
        string toString(bool includePort=true) const;

        /** 
         * @return one of AF_INET, AF_INET6, or AF_UNIX
         */
        sa_family_t getType() const;

        unsigned getPort() const;

        string getAddr() const;

        bool isLocalHost() const;

        bool operator==(const SockAddr& r) const;

        bool operator!=(const SockAddr& r) const;

        bool operator<(const SockAddr& r) const;

        const sockaddr* raw() const {return (sockaddr*)&sa;}
        sockaddr* raw() {return (sockaddr*)&sa;}

        socklen_t addressSize;
    private:
        struct sockaddr_storage sa;
    };

    extern SockAddr unknownAddress; // ( "0.0.0.0", 0 )

    /** this is not cache and does a syscall */
    string getHostName();
    
    /** this is cached, so if changes during the process lifetime
     * will be stale */
    string getHostNameCached();

    /**
     * thrown by Socket and SockAddr
     */
    class SocketException : public DBException {
    public:
        const enum Type { CLOSED , RECV_ERROR , SEND_ERROR, RECV_TIMEOUT, SEND_TIMEOUT, FAILED_STATE, CONNECT_ERROR } _type;
        
        SocketException( Type t , string server , int code = 9001 , string extra="" ) 
            : DBException( "socket exception" , code ) , _type(t) , _server(server), _extra(extra){ }
        virtual ~SocketException() throw() {}

        bool shouldPrint() const { return _type != CLOSED; }
        virtual string toString() const;

    private:
        string _server;
        string _extra;
    };

    /**
     * thin wrapped around file descriptor and system calls
     * todo: ssl
     */
    class Socket {
    public:
        Socket(int sock, const SockAddr& farEnd);

        // in some cases the timeout will actually be 2x this value - eg we do a partial send,
        // then the timeout fires, then we try to send again, then the timeout fires again with
        // no data sent, then we detect that the other side is down
        Socket(double so_timeout = 0, int logLevel = 0 );

        bool connect(SockAddr& farEnd);
        void close();
        
        void send( const char * data , int len, const char *context );
        void send( const vector< pair< char *, int > > &data, const char *context );

        // recv len or throw SocketException
        void recv( char * data , int len );
        int unsafe_recv( char *buf, int max );
        
        int getLogLevel() const { return _logLevel; }
        void setLogLevel( int ll ) { _logLevel = ll; }

        SockAddr remoteAddr() const { return _remote; }
        string remoteString() const { return _remote.toString(); }
        unsigned remotePort() const { return _remote.getPort(); }

        void clearCounters() { _bytesIn = 0; _bytesOut = 0; }
        long long getBytesIn() const { return _bytesIn; }
        long long getBytesOut() const { return _bytesOut; }

    private:
        int _fd;
        SockAddr _remote;
        double _timeout;

        long long _bytesIn;
        long long _bytesOut;

    protected:
        int _logLevel; // passed to log() when logging errors

    };


} // namespace mongo
