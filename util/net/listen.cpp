// listen.h

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


#include "pch.h"
#include "listen.h"
#include "message_port.h"

#ifndef _WIN32

# ifndef __sunos__
#  include <ifaddrs.h>
# endif
# include <sys/resource.h>
# include <sys/stat.h>

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

#else

// errno doesn't work for winsock.
#undef errno
#define errno WSAGetLastError()

#endif

namespace mongo {


    void checkTicketNumbers();

    
    // ----- Listener -------

    const Listener* Listener::_timeTracker;

    vector<SockAddr> ipToAddrs(const char* ips, int port, bool useUnixSockets) {
        vector<SockAddr> out;
        if (*ips == '\0') {
            out.push_back(SockAddr("0.0.0.0", port)); // IPv4 all

            if (IPv6Enabled())
                out.push_back(SockAddr("::", port)); // IPv6 all
#ifndef _WIN32
            if (useUnixSockets)
                out.push_back(SockAddr(makeUnixSockPath(port).c_str(), port)); // Unix socket
#endif
            return out;
        }

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

            SockAddr sa(ip.c_str(), port);
            out.push_back(sa);

#ifndef _WIN32
            if (useUnixSockets && (sa.getAddr() == "127.0.0.1" || sa.getAddr() == "0.0.0.0")) // only IPv4
                out.push_back(SockAddr(makeUnixSockPath(port).c_str(), port));
#endif
        }
        return out;

    }

    void Listener::initAndListen() {
        checkTicketNumbers();
        vector<SockAddr> mine = ipToAddrs(_ip.c_str(), _port, (!cmdLine.noUnixSocket && useUnixSockets()));
        vector<int> socks;
        SOCKET maxfd = 0; // needed for select()

        for (vector<SockAddr>::iterator it=mine.begin(), end=mine.end(); it != end; ++it) {
            SockAddr& me = *it;

            SOCKET sock = ::socket(me.getType(), SOCK_STREAM, 0);
            if ( sock == INVALID_SOCKET ) {
                log() << "ERROR: listen(): invalid socket? " << errnoWithDescription() << endl;
            }

            if (me.getType() == AF_UNIX) {
#if !defined(_WIN32)
                if (unlink(me.getAddr().c_str()) == -1) {
                    int x = errno;
                    if (x != ENOENT) {
                        log() << "couldn't unlink socket file " << me << errnoWithDescription(x) << " skipping" << endl;
                        continue;
                    }
                }
#endif
            }
            else if (me.getType() == AF_INET6) {
                // IPv6 can also accept IPv4 connections as mapped addresses (::ffff:127.0.0.1)
                // That causes a conflict if we don't do set it to IPV6_ONLY
                const int one = 1;
                setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char*) &one, sizeof(one));
            }

#if !defined(_WIN32)
            {
                int one;
                if ( setsockopt( sock , SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0 )
                    out() << "Failed to set socket opt, SO_REUSEADDR" << endl;
            }
#endif

            if ( ::bind(sock, me.raw(), me.addressSize) != 0 ) {
                int x = errno;
                log() << "listen(): bind() failed " << errnoWithDescription(x) << " for socket: " << me.toString() << endl;
                if ( x == EADDRINUSE )
                    log() << "  addr already in use" << endl;
                closesocket(sock);
                return;
            }

#if !defined(_WIN32)
            if (me.getType() == AF_UNIX) {
                if (chmod(me.getAddr().c_str(), 0777) == -1) {
                    log() << "couldn't chmod socket file " << me << errnoWithDescription() << endl;
                }

                ListeningSockets::get()->addPath( me.getAddr() );
            }
#endif

            if ( ::listen(sock, 128) != 0 ) {
                log() << "listen(): listen() failed " << errnoWithDescription() << endl;
                closesocket(sock);
                return;
            }

            ListeningSockets::get()->add( sock );

            socks.push_back(sock);
            if (sock > maxfd)
                maxfd = sock;
        }

        static long connNumber = 0;
        struct timeval maxSelectTime;
        while ( ! inShutdown() ) {
            fd_set fds[1];
            FD_ZERO(fds);

            for (vector<int>::iterator it=socks.begin(), end=socks.end(); it != end; ++it) {
                FD_SET(*it, fds);
            }

            maxSelectTime.tv_sec = 0;
            maxSelectTime.tv_usec = 10000;
            const int ret = select(maxfd+1, fds, NULL, NULL, &maxSelectTime);

            if (ret == 0) {
#if defined(__linux__)
                _elapsedTime += ( 10000 - maxSelectTime.tv_usec ) / 1000;
#else
                _elapsedTime += 10;
#endif
                continue;
            }

            if (ret < 0) {
                int x = errno;
#ifdef EINTR
                if ( x == EINTR ) {
                    log() << "select() signal caught, continuing" << endl;
                    continue;
                }
#endif
                if ( ! inShutdown() )
                    log() << "select() failure: ret=" << ret << " " << errnoWithDescription(x) << endl;
                return;
            }

#if defined(__linux__)
            _elapsedTime += max(ret, (int)(( 10000 - maxSelectTime.tv_usec ) / 1000));
#else
            _elapsedTime += ret; // assume 1ms to grab connection. very rough
#endif

            for (vector<int>::iterator it=socks.begin(), end=socks.end(); it != end; ++it) {
                if (! (FD_ISSET(*it, fds)))
                    continue;

                SockAddr from;
                int s = accept(*it, from.raw(), &from.addressSize);
                if ( s < 0 ) {
                    int x = errno; // so no global issues
                    if ( x == ECONNABORTED || x == EBADF ) {
                        log() << "Listener on port " << _port << " aborted" << endl;
                        return;
                    }
                    if ( x == 0 && inShutdown() ) {
                        return;   // socket closed
                    }
                    if( !inShutdown() )
                        log() << "Listener: accept() returns " << s << " " << errnoWithDescription(x) << endl;
                    continue;
                }
                if (from.getType() != AF_UNIX)
                    disableNagle(s);
                if ( _logConnect && ! cmdLine.quiet )
                    log() << "connection accepted from " << from.toString() << " #" << ++connNumber << endl;
                accepted(s, from);
            }
        }
    }

    void Listener::accepted(int sock, const SockAddr& from) {
        accepted( new MessagingPort(sock, from) );
    }
    
    void Listener::accepted(MessagingPort *mp) {
        assert(!"You must overwrite one of the accepted methods");
    }

    // ----- ListeningSockets -------

    ListeningSockets* ListeningSockets::_instance = new ListeningSockets();

    ListeningSockets* ListeningSockets::get() {
        return _instance;
    }

    // ------ connection ticket and control ------

    const int DEFAULT_MAX_CONN = 20000;
    const int MAX_MAX_CONN = 20000;

    int getMaxConnections() {
#ifdef _WIN32
        return DEFAULT_MAX_CONN;
#else
        struct rlimit limit;
        assert( getrlimit(RLIMIT_NOFILE,&limit) == 0 );

        int max = (int)(limit.rlim_cur * .8);

        log(1) << "fd limit"
               << " hard:" << limit.rlim_max
               << " soft:" << limit.rlim_cur
               << " max conn: " << max
               << endl;

        if ( max > MAX_MAX_CONN )
            max = MAX_MAX_CONN;

        return max;
#endif
    }

    void checkTicketNumbers() {
        int want = getMaxConnections();
        int current = connTicketHolder.outof();
        if ( current != DEFAULT_MAX_CONN ) {
            if ( current < want ) {
                // they want fewer than they can handle
                // which is fine
                log(1) << " only allowing " << current << " connections" << endl;
                return;
            }
            if ( current > want ) {
                log() << " --maxConns too high, can only handle " << want << endl;
            }
        }
        connTicketHolder.resize( want );
    }

    TicketHolder connTicketHolder(DEFAULT_MAX_CONN);

}
