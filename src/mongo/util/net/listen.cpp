// listen.cpp

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


#include "mongo/pch.h"

#include "mongo/util/net/listen.h"

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/util/net/message_port.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/scopeguard.h"

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
    
    Listener::Listener(const string& name, const string &ip, int port, bool logConnect ) 
        : _port(port), _name(name), _ip(ip), _setupSocketsSuccessful(false),
          _logConnect(logConnect), _elapsedTime(0) {
#ifdef MONGO_SSL
        _ssl = getSSLManager();
#endif
    }
    
    Listener::~Listener() {
        if ( _timeTracker == this )
            _timeTracker = 0;
    }

    void Listener::setupSockets() {
        checkTicketNumbers();

#if !defined(_WIN32)
        _mine = ipToAddrs(_ip.c_str(), _port, (!cmdLine.noUnixSocket && useUnixSockets()));
#else
        _mine = ipToAddrs(_ip.c_str(), _port, false);
#endif

        for (vector<SockAddr>::const_iterator it=_mine.begin(), end=_mine.end(); it != end; ++it) {
            const SockAddr& me = *it;

            SOCKET sock = ::socket(me.getType(), SOCK_STREAM, 0);
            ScopeGuard socketGuard = MakeGuard(&closesocket, sock);
            massert( 15863 , str::stream() << "listen(): invalid socket? " << errnoWithDescription() , sock >= 0 );

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
                const int one = 1;
                if ( setsockopt( sock , SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0 )
                    out() << "Failed to set socket opt, SO_REUSEADDR" << endl;
            }
#endif

            if ( ::bind(sock, me.raw(), me.addressSize) != 0 ) {
                int x = errno;
                error() << "listen(): bind() failed " << errnoWithDescription(x) << " for socket: " << me.toString() << endl;
                if ( x == EADDRINUSE )
                    error() << "  addr already in use" << endl;
                return;
            }

#if !defined(_WIN32)
            if (me.getType() == AF_UNIX) {
                if (chmod(me.getAddr().c_str(), 0777) == -1) {
                    error() << "couldn't chmod socket file " << me << errnoWithDescription() << endl;
                }
                ListeningSockets::get()->addPath( me.getAddr() );
            }
#endif
            
            if ( ::listen(sock, 128) != 0 ) {
                error() << "listen(): listen() failed " << errnoWithDescription() << endl;
                return;
            }

            ListeningSockets::get()->add( sock );

            _socks.push_back(sock);
            socketGuard.Dismiss();
        }
        
        _setupSocketsSuccessful = true;
    }
    
 
#if !defined(_WIN32)
    void Listener::initAndListen() {
        if (!_setupSocketsSuccessful) {
            return;
        }

        SOCKET maxfd = 0; // needed for select()
        for (unsigned i = 0; i < _socks.size(); i++) {
            if (_socks[i] > maxfd)
                maxfd = _socks[i];
        }

        if ( maxfd >= FD_SETSIZE ) {
            error() << "socket " << maxfd << " is higher than " << FD_SETSIZE-1 << 
                "; not supported" << warnings;
            return;
        }

#ifdef MONGO_SSL
        _logListen(_port, _ssl);
#else
        _logListen(_port, false);
#endif

        struct timeval maxSelectTime;
        while ( ! inShutdown() ) {
            fd_set fds[1];
            FD_ZERO(fds);
            
            for (vector<SOCKET>::iterator it=_socks.begin(), end=_socks.end(); it != end; ++it) {
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

            for (vector<SOCKET>::iterator it=_socks.begin(), end=_socks.end(); it != end; ++it) {
                if (! (FD_ISSET(*it, fds)))
                    continue;
                SockAddr from;
                int s = accept(*it, from.raw(), &from.addressSize);
                if ( s < 0 ) {
                    int x = errno; // so no global issues
                    if (x == EBADF) {
                        log() << "Port " << _port << " is no longer valid" << endl;
                        return;
                    }
                    else if (x == ECONNABORTED) {
                        log() << "Connection on port " << _port << " aborted" << endl;
                        continue;
                    }
                    if ( x == 0 && inShutdown() ) {
                        return;   // socket closed
                    }
                    if( !inShutdown() ) {
                        log() << "Listener: accept() returns " << s << " " << errnoWithDescription(x) << endl;
                        if (x == EMFILE || x == ENFILE) {
                            // Connection still in listen queue but we can't accept it yet
                            error() << "Out of file descriptors. Waiting one second before trying to accept more connections." << warnings;
                            sleepsecs(1);
                        }
                    }
                    continue;
                }
                if (from.getType() != AF_UNIX)
                    disableNagle(s);

#ifdef SO_NOSIGPIPE
                // ignore SIGPIPE signals on osx, to avoid process exit
                const int one = 1;
                setsockopt( s , SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(int));
#endif

                long long myConnectionNumber = globalConnectionNumber.addAndFetch(1);

                if ( _logConnect && ! cmdLine.quiet ){
                    int conns = globalTicketHolder.used()+1;
                    const char* word = (conns == 1 ? " connection" : " connections");
                    log() << "connection accepted from " << from.toString() << " #" << myConnectionNumber << " (" << conns << word << " now open)" << endl;
                }
                
                boost::shared_ptr<Socket> pnewSock( new Socket(s, from) );
#ifdef MONGO_SSL
                if (_ssl) {
                    pnewSock->secureAccepted(_ssl);
                }
#endif
                accepted( pnewSock , myConnectionNumber );
            }
        }
    }

#else 
    // Windows    
    
    // Given a SOCKET, turns off nonblocking mode
    static void disableNonblockingMode(SOCKET socket) {
        unsigned long resultBuffer = 0;
        unsigned long resultBufferBytesWritten = 0;
        unsigned long newNonblockingEnabled = 0;
        const int status = WSAIoctl(socket, 
                                    FIONBIO, 
                                    &newNonblockingEnabled, 
                                    sizeof(unsigned long), 
                                    &resultBuffer, 
                                    sizeof(resultBuffer), 
                                    &resultBufferBytesWritten, 
                                    NULL, 
                                    NULL);
        if (status == SOCKET_ERROR) {
            const int mongo_errno = WSAGetLastError();
            error() << "Windows WSAIoctl returned " << errnoWithDescription(mongo_errno) << endl;
            fassertFailed(16726);
        }
    }

    // RAII wrapper class to ensure we do not leak WSAEVENTs.
    class EventHolder {
        WSAEVENT _socketEventHandle;
    public:
        EventHolder() {
            _socketEventHandle = WSACreateEvent();
            if (_socketEventHandle == WSA_INVALID_EVENT) {
                const int mongo_errno = WSAGetLastError();
                error() << "Windows WSACreateEvent returned " << errnoWithDescription(mongo_errno) 
                    << endl;
                fassertFailed(16728);
            }
        }
        ~EventHolder() {
            BOOL bstatus = WSACloseEvent(_socketEventHandle);
            if (bstatus == FALSE) {
                const int mongo_errno = WSAGetLastError();
                error() << "Windows WSACloseEvent returned " << errnoWithDescription(mongo_errno)
                    << endl;
                fassertFailed(16725);
            }        
        }
        WSAEVENT get() {
            return _socketEventHandle;
        }
    };
    
    void Listener::initAndListen() {
        if (!_setupSocketsSuccessful) {
            return;
        }

#ifdef MONGO_SSL
        _logListen(_port, _ssl);
#else
        _logListen(_port, false);
#endif
                
        OwnedPointerVector<EventHolder> eventHolders;
        boost::scoped_array<WSAEVENT> events(new WSAEVENT[_socks.size()]);
        
        
        // Populate events array with an event for each socket we are watching
        for (size_t count = 0; count < _socks.size(); ++count) {
            EventHolder* ev(new EventHolder);
            eventHolders.mutableVector().push_back(ev);
            events[count] = ev->get();            
        }
            
        while ( ! inShutdown() ) {
            // Turn on listening for accept-ready sockets
            for (size_t count = 0; count < _socks.size(); ++count) {
                int status = WSAEventSelect(_socks[count], events[count], FD_ACCEPT | FD_CLOSE);
                if (status == SOCKET_ERROR) {
                    const int mongo_errno = WSAGetLastError();
                    error() << "Windows WSAEventSelect returned " 
                        << errnoWithDescription(mongo_errno) << endl;
                    fassertFailed(16727);
                }
            }
        
            // Wait till one of them goes active, or we time out
            DWORD result = WSAWaitForMultipleEvents(_socks.size(),
                                                    events.get(), 
                                                    FALSE, // don't wait for all the events
                                                    10, // timeout, in ms 
                                                    FALSE); // do not allow I/O interruptions
            if (result == WSA_WAIT_FAILED) {
                const int mongo_errno = WSAGetLastError();
                error() << "Windows WSAWaitForMultipleEvents returned " 
                    << errnoWithDescription(mongo_errno) << endl;
                fassertFailed(16723);
            }
        
            if (result == WSA_WAIT_TIMEOUT) {
                _elapsedTime += 10;
                continue;
            }
            _elapsedTime += 1; // assume 1ms to grab connection. very rough
            
            // Determine which socket is ready
            DWORD eventIndex = result - WSA_WAIT_EVENT_0;
            WSANETWORKEVENTS networkEvents;            
            // Extract event details, and clear event for next pass
            int status = WSAEnumNetworkEvents(_socks[eventIndex],
                                              events[eventIndex], 
                                              &networkEvents);
            if (status == SOCKET_ERROR) {
                const int mongo_errno = WSAGetLastError();
                error() << "Windows WSAEnumNetworkEvents returned " 
                    << errnoWithDescription(mongo_errno) << endl;
                continue;
            }
            
            if (networkEvents.lNetworkEvents & FD_CLOSE) {              
                log() << "listen socket closed" << endl;
                break;
            }
            
            if (!(networkEvents.lNetworkEvents & FD_ACCEPT)) {
                error() << "Unexpected network event: " << networkEvents.lNetworkEvents << endl;
                continue;
            }
            
            int iec = networkEvents.iErrorCode[FD_ACCEPT_BIT];
            if (iec != 0) {                 
                error() << "Windows socket accept did not work:" << errnoWithDescription(iec) 
                        << endl;
                continue;
            }
            
            status = WSAEventSelect(_socks[eventIndex], NULL, 0);
            if (status == SOCKET_ERROR) {
                const int mongo_errno = WSAGetLastError();
                error() << "Windows WSAEventSelect returned " 
                    << errnoWithDescription(mongo_errno) << endl;
                continue;
            }
            
            disableNonblockingMode(_socks[eventIndex]);
            
            SockAddr from;
            int s = accept(_socks[eventIndex], from.raw(), &from.addressSize);
            if ( s < 0 ) {
                int x = errno; // so no global issues
                if (x == EBADF) {
                    log() << "Port " << _port << " is no longer valid" << endl;
                    continue;
                }
                else if (x == ECONNABORTED) {
                    log() << "Listener on port " << _port << " aborted" << endl;
                    continue;
                }
                if ( x == 0 && inShutdown() ) {
                    return;   // socket closed
                }
                if( !inShutdown() ) {
                    log() << "Listener: accept() returns " << s << " " 
                        << errnoWithDescription(x) << endl;
                    if (x == EMFILE || x == ENFILE) {
                        // Connection still in listen queue but we can't accept it yet
                        error() << "Out of file descriptors. Waiting one second before"
                            " trying to accept more connections." << warnings;
                        sleepsecs(1);
                    }
                }
                continue;
            }
            if (from.getType() != AF_UNIX)
                disableNagle(s);

            long long myConnectionNumber = globalConnectionNumber.addAndFetch(1);

            if ( _logConnect && ! cmdLine.quiet ){
                int conns = globalTicketHolder.used()+1;
                const char* word = (conns == 1 ? " connection" : " connections");
                log() << "connection accepted from " << from.toString() << " #" << myConnectionNumber << " (" << conns << word << " now open)" << endl;
            }
            
            boost::shared_ptr<Socket> pnewSock( new Socket(s, from) );
#ifdef MONGO_SSL
            if (_ssl) {
                pnewSock->secureAccepted(_ssl);
            }
#endif
            accepted( pnewSock , myConnectionNumber );
        }
    }
#endif

    void Listener::_logListen( int port , bool ssl ) {
        log() << _name << ( _name.size() ? " " : "" ) << "waiting for connections on port " << port << ( ssl ? " ssl" : "" ) << endl;
    }


    void Listener::accepted(boost::shared_ptr<Socket> psocket, long long connectionId ) {
        MessagingPort* port = new MessagingPort(psocket);
        port->setConnectionId( connectionId );
        acceptedMP( port );
    }
    
    void Listener::acceptedMP(MessagingPort *mp) {
        verify(!"You must overwrite one of the accepted methods");
    }

    // ----- ListeningSockets -------

    ListeningSockets* ListeningSockets::_instance = new ListeningSockets();

    ListeningSockets* ListeningSockets::get() {
        return _instance;
    }

    // ------ connection ticket and control ------

    int getMaxConnections() {
#ifdef _WIN32
        return DEFAULT_MAX_CONN;
#else
        struct rlimit limit;
        verify( getrlimit(RLIMIT_NOFILE,&limit) == 0 );

        int max = (int)(limit.rlim_cur * .8);

        LOG(1) << "fd limit"
               << " hard:" << limit.rlim_max
               << " soft:" << limit.rlim_cur
               << " max conn: " << max
               << endl;

        return max;
#endif
    }

    void Listener::checkTicketNumbers() {
        int want = getMaxConnections();
        int current = globalTicketHolder.outof();
        if ( current != DEFAULT_MAX_CONN ) {
            if ( current < want ) {
                // they want fewer than they can handle
                // which is fine
                LOG(1) << " only allowing " << current << " connections" << endl;
                return;
            }
            if ( current > want ) {
                log() << " --maxConns too high, can only handle " << want << endl;
            }
        }
        globalTicketHolder.resize( want );
    }


    TicketHolder Listener::globalTicketHolder(DEFAULT_MAX_CONN);
    AtomicInt64 Listener::globalConnectionNumber;
}
