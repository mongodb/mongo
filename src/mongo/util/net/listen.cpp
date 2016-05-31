// listen.cpp

/*    Copyright 2009 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */


#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/listen.h"

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/asio_message_port.h"
#include "mongo/util/net/message_port.h"
#include "mongo/util/net/message_port_startup_param.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/scopeguard.h"

#ifndef _WIN32

#ifndef __sun
#include <ifaddrs.h>
#endif
#include <sys/resource.h>
#include <sys/stat.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#ifdef __OpenBSD__
#include <sys/uio.h>
#endif

#else

// errno doesn't work for winsock.
#undef errno
#define errno WSAGetLastError()

#endif

namespace mongo {

namespace {
const auto getListener = ServiceContext::declareDecoration<Listener*>();
}  // namespace

using std::shared_ptr;
using std::endl;
using std::string;
using std::vector;

// ----- Listener -------

vector<SockAddr> ipToAddrs(const char* ips, int port, bool useUnixSockets) {
    vector<SockAddr> out;
    if (*ips == '\0') {
        out.push_back(SockAddr("0.0.0.0", port));  // IPv4 all

        if (IPv6Enabled())
            out.push_back(SockAddr("::", port));  // IPv6 all
#ifndef _WIN32
        if (useUnixSockets)
            out.push_back(SockAddr(makeUnixSockPath(port), port));  // Unix socket
#endif
        return out;
    }

    while (*ips) {
        string ip;
        const char* comma = strchr(ips, ',');
        if (comma) {
            ip = string(ips, comma - ips);
            ips = comma + 1;
        } else {
            ip = string(ips);
            ips = "";
        }

        SockAddr sa(ip.c_str(), port);
        out.push_back(sa);

#ifndef _WIN32
        if (sa.isValid() && useUnixSockets &&
            (sa.getAddr() == "127.0.0.1" || sa.getAddr() == "0.0.0.0"))  // only IPv4
            out.push_back(SockAddr(makeUnixSockPath(port), port));
#endif
    }
    return out;
}

Listener* Listener::get(ServiceContext* ctx) {
    return getListener(ctx);
}

Listener::Listener(const string& name,
                   const string& ip,
                   int port,
                   ServiceContext* ctx,
                   bool setAsServiceCtxDecoration,
                   bool logConnect)
    : _port(port),
      _name(name),
      _ip(ip),
      _setupSocketsSuccessful(false),
      _logConnect(logConnect),
      _ready(false),
      _ctx(ctx),
      _setAsServiceCtxDecoration(setAsServiceCtxDecoration) {
#ifdef MONGO_CONFIG_SSL
    _ssl = getSSLManager();
#endif
    if (setAsServiceCtxDecoration) {
        auto& listener = getListener(ctx);
        invariant(!listener);
        listener = this;
    }
}

Listener::~Listener() {
    if (_setAsServiceCtxDecoration) {
        auto& listener = getListener(_ctx);
        listener = nullptr;
    }
}

bool Listener::setupSockets() {
    checkTicketNumbers();

#if !defined(_WIN32)
    _mine = ipToAddrs(_ip.c_str(), _port, (!serverGlobalParams.noUnixSocket && useUnixSockets()));
#else
    _mine = ipToAddrs(_ip.c_str(), _port, false);
#endif

    for (std::vector<SockAddr>::const_iterator it = _mine.begin(), end = _mine.end(); it != end;
         ++it) {
        const SockAddr& me = *it;

        if (!me.isValid()) {
            error() << "listen(): socket is invalid." << endl;
            return _setupSocketsSuccessful;
        }

        SOCKET sock = ::socket(me.getType(), SOCK_STREAM, 0);
        ScopeGuard socketGuard = MakeGuard(&closesocket, sock);
        massert(15863,
                str::stream() << "listen(): invalid socket? " << errnoWithDescription(),
                sock >= 0);

        if (me.getType() == AF_UNIX) {
#if !defined(_WIN32)
            if (unlink(me.getAddr().c_str()) == -1) {
                if (errno != ENOENT) {
                    error() << "Failed to unlink socket file " << me << " "
                            << errnoWithDescription(errno);
                    fassertFailedNoTrace(28578);
                }
            }
#endif
        } else if (me.getType() == AF_INET6) {
            // IPv6 can also accept IPv4 connections as mapped addresses (::ffff:127.0.0.1)
            // That causes a conflict if we don't do set it to IPV6_ONLY
            const int one = 1;
            setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&one, sizeof(one));
        }

#if !defined(_WIN32)
        {
            const int one = 1;
            if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
                log() << "Failed to set socket opt, SO_REUSEADDR" << endl;
        }
#endif

        if (::bind(sock, me.raw(), me.addressSize) != 0) {
            int x = errno;
            error() << "listen(): bind() failed " << errnoWithDescription(x)
                    << " for socket: " << me.toString() << endl;
            if (x == EADDRINUSE)
                error() << "  addr already in use" << endl;
            return _setupSocketsSuccessful;
        }

#if !defined(_WIN32)
        if (me.getType() == AF_UNIX) {
            if (chmod(me.getAddr().c_str(), serverGlobalParams.unixSocketPermissions) == -1) {
                error() << "Failed to chmod socket file " << me << " "
                        << errnoWithDescription(errno);
                fassertFailedNoTrace(28582);
            }
            ListeningSockets::get()->addPath(me.getAddr());
        }
#endif

        _socks.push_back(sock);
        socketGuard.Dismiss();
    }

    _setupSocketsSuccessful = true;
    return _setupSocketsSuccessful;
}


#if !defined(_WIN32)
void Listener::initAndListen() {
    if (!_setupSocketsSuccessful) {
        return;
    }

    SOCKET maxfd = 0;  // needed for select()
    for (unsigned i = 0; i < _socks.size(); i++) {
        if (::listen(_socks[i], 128) != 0) {
            error() << "listen(): listen() failed " << errnoWithDescription() << endl;
            return;
        }

        ListeningSockets::get()->add(_socks[i]);

        if (_socks[i] > maxfd) {
            maxfd = _socks[i];
        }
    }

    if (maxfd >= FD_SETSIZE) {
        error() << "socket " << maxfd << " is higher than " << FD_SETSIZE - 1 << "; not supported"
                << warnings;
        return;
    }

#ifdef MONGO_CONFIG_SSL
    _logListen(_port, _ssl);
#else
    _logListen(_port, false);
#endif

    {
        // Wake up any threads blocked in waitUntilListening()
        stdx::lock_guard<stdx::mutex> lock(_readyMutex);
        _ready = true;
        _readyCondition.notify_all();
    }

    struct timeval maxSelectTime;
    // The check against _finished allows us to actually stop the listener by signalling it through
    // the _finished flag.
    while (!inShutdown() && !_finished.load()) {
        fd_set fds[1];
        FD_ZERO(fds);

        for (vector<SOCKET>::iterator it = _socks.begin(), end = _socks.end(); it != end; ++it) {
            FD_SET(*it, fds);
        }

        maxSelectTime.tv_sec = 0;
        maxSelectTime.tv_usec = 250000;
        const int ret = select(maxfd + 1, fds, nullptr, nullptr, &maxSelectTime);

        if (ret == 0) {
            continue;
        } else if (ret < 0) {
            int x = errno;
#ifdef EINTR
            if (x == EINTR) {
                log() << "select() signal caught, continuing" << endl;
                continue;
            }
#endif
            if (!inShutdown())
                log() << "select() failure: ret=" << ret << " " << errnoWithDescription(x) << endl;
            return;
        }

        for (vector<SOCKET>::iterator it = _socks.begin(), end = _socks.end(); it != end; ++it) {
            if (!(FD_ISSET(*it, fds)))
                continue;
            SockAddr from;
            int s = accept(*it, from.raw(), &from.addressSize);
            if (s < 0) {
                int x = errno;  // so no global issues
                if (x == EBADF) {
                    log() << "Port " << _port << " is no longer valid" << endl;
                    return;
                } else if (x == ECONNABORTED) {
                    log() << "Connection on port " << _port << " aborted" << endl;
                    continue;
                }
                if (x == 0 && inShutdown()) {
                    return;  // socket closed
                }
                if (!inShutdown()) {
                    log() << "Listener: accept() returns " << s << " " << errnoWithDescription(x)
                          << endl;
                    if (x == EMFILE || x == ENFILE) {
                        // Connection still in listen queue but we can't accept it yet
                        error() << "Out of file descriptors. Waiting one second before trying to "
                                   "accept more connections."
                                << warnings;
                        sleepsecs(1);
                    }
                }
                continue;
            }

            long long myConnectionNumber = globalConnectionNumber.addAndFetch(1);

            if (_logConnect && !serverGlobalParams.quiet) {
                int conns = globalTicketHolder.used() + 1;
                const char* word = (conns == 1 ? " connection" : " connections");
                log() << "connection accepted from " << from.toString() << " #"
                      << myConnectionNumber << " (" << conns << word << " now open)" << endl;
            }

            if (from.getType() != AF_UNIX)
                disableNagle(s);

#ifdef SO_NOSIGPIPE
            // ignore SIGPIPE signals on osx, to avoid process exit
            const int one = 1;
            setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(int));
#endif

            std::shared_ptr<Socket> pnewSock(new Socket(s, from));
#ifdef MONGO_CONFIG_SSL
            if (_ssl) {
                pnewSock->secureAccepted(_ssl);
            }
#endif
            _accepted(pnewSock, myConnectionNumber);
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

    for (unsigned i = 0; i < _socks.size(); i++) {
        if (::listen(_socks[i], 128) != 0) {
            error() << "listen(): listen() failed " << errnoWithDescription() << endl;
            return;
        }

        ListeningSockets::get()->add(_socks[i]);
    }

#ifdef MONGO_CONFIG_SSL
    _logListen(_port, _ssl);
#else
    _logListen(_port, false);
#endif

    {
        // Wake up any threads blocked in waitUntilListening()
        stdx::lock_guard<stdx::mutex> lock(_readyMutex);
        _ready = true;
        _readyCondition.notify_all();
    }

    OwnedPointerVector<EventHolder> eventHolders;
    std::unique_ptr<WSAEVENT[]> events(new WSAEVENT[_socks.size()]);


    // Populate events array with an event for each socket we are watching
    for (size_t count = 0; count < _socks.size(); ++count) {
        EventHolder* ev(new EventHolder);
        eventHolders.mutableVector().push_back(ev);
        events[count] = ev->get();
    }

    while (!inShutdown()) {
        // Turn on listening for accept-ready sockets
        for (size_t count = 0; count < _socks.size(); ++count) {
            int status = WSAEventSelect(_socks[count], events[count], FD_ACCEPT | FD_CLOSE);
            if (status == SOCKET_ERROR) {
                const int mongo_errno = WSAGetLastError();

                // We may fail to listen on the socket if it has
                // already been closed. If we are not in shutdown,
                // that is possibly interesting, so log an error.
                if (!inShutdown()) {
                    error() << "Windows WSAEventSelect returned "
                            << errnoWithDescription(mongo_errno) << endl;
                }

                return;
            }
        }

        // Wait till one of them goes active, or we time out
        DWORD result = WSAWaitForMultipleEvents(_socks.size(),
                                                events.get(),
                                                FALSE,   // don't wait for all the events
                                                250,     // timeout, in ms
                                                FALSE);  // do not allow I/O interruptions

        if (result == WSA_WAIT_TIMEOUT) {
            continue;
        } else if (result == WSA_WAIT_FAILED) {
            const int mongo_errno = WSAGetLastError();
            error() << "Windows WSAWaitForMultipleEvents returned "
                    << errnoWithDescription(mongo_errno) << endl;
            fassertFailed(16723);
        }

        // Determine which socket is ready
        DWORD eventIndex = result - WSA_WAIT_EVENT_0;
        WSANETWORKEVENTS networkEvents;
        // Extract event details, and clear event for next pass
        int status = WSAEnumNetworkEvents(_socks[eventIndex], events[eventIndex], &networkEvents);
        if (status == SOCKET_ERROR) {
            const int mongo_errno = WSAGetLastError();
            error() << "Windows WSAEnumNetworkEvents returned " << errnoWithDescription(mongo_errno)
                    << endl;
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
            error() << "Windows socket accept did not work:" << errnoWithDescription(iec) << endl;
            continue;
        }

        status = WSAEventSelect(_socks[eventIndex], NULL, 0);
        if (status == SOCKET_ERROR) {
            const int mongo_errno = WSAGetLastError();
            error() << "Windows WSAEventSelect returned " << errnoWithDescription(mongo_errno)
                    << endl;
            continue;
        }

        disableNonblockingMode(_socks[eventIndex]);

        SockAddr from;
        int s = accept(_socks[eventIndex], from.raw(), &from.addressSize);
        if (s < 0) {
            int x = errno;  // so no global issues
            if (x == EBADF) {
                log() << "Port " << _port << " is no longer valid" << endl;
                continue;
            } else if (x == ECONNABORTED) {
                log() << "Listener on port " << _port << " aborted" << endl;
                continue;
            }
            if (x == 0 && inShutdown()) {
                return;  // socket closed
            }
            if (!inShutdown()) {
                log() << "Listener: accept() returns " << s << " " << errnoWithDescription(x)
                      << endl;
                if (x == EMFILE || x == ENFILE) {
                    // Connection still in listen queue but we can't accept it yet
                    error() << "Out of file descriptors. Waiting one second before"
                               " trying to accept more connections."
                            << warnings;
                    sleepsecs(1);
                }
            }
            continue;
        }

        long long myConnectionNumber = globalConnectionNumber.addAndFetch(1);

        if (_logConnect && !serverGlobalParams.quiet) {
            int conns = globalTicketHolder.used() + 1;
            const char* word = (conns == 1 ? " connection" : " connections");
            log() << "connection accepted from " << from.toString() << " #" << myConnectionNumber
                  << " (" << conns << word << " now open)" << endl;
        }

        if (from.getType() != AF_UNIX)
            disableNagle(s);

        std::shared_ptr<Socket> pnewSock(new Socket(s, from));
#ifdef MONGO_CONFIG_SSL
        if (_ssl) {
            pnewSock->secureAccepted(_ssl);
        }
#endif
        _accepted(pnewSock, myConnectionNumber);
    }
}
#endif

void Listener::_logListen(int port, bool ssl) {
    log() << _name << (_name.size() ? " " : "") << "waiting for connections on port " << port
          << (ssl ? " ssl" : "") << endl;
}

void Listener::waitUntilListening() const {
    stdx::unique_lock<stdx::mutex> lock(_readyMutex);
    while (!_ready) {
        _readyCondition.wait(lock);
    }
}

void Listener::_accepted(const std::shared_ptr<Socket>& psocket, long long connectionId) {
    std::unique_ptr<AbstractMessagingPort> port;
    if (isMessagePortImplASIO()) {
        port = stdx::make_unique<ASIOMessagingPort>(psocket->stealSD(), psocket->remoteAddr());
    } else {
        port = stdx::make_unique<MessagingPort>(psocket);
    }
    port->setConnectionId(connectionId);
    accepted(std::move(port));
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
    verify(getrlimit(RLIMIT_NOFILE, &limit) == 0);

    int max = (int)(limit.rlim_cur * .8);

    LOG(1) << "fd limit"
           << " hard:" << limit.rlim_max << " soft:" << limit.rlim_cur << " max conn: " << max
           << endl;

    return max;
#endif
}

void Listener::checkTicketNumbers() {
    int want = getMaxConnections();
    int current = globalTicketHolder.outof();
    if (current != DEFAULT_MAX_CONN) {
        if (current < want) {
            // they want fewer than they can handle
            // which is fine
            LOG(1) << " only allowing " << current << " connections" << endl;
            return;
        }
        if (current > want) {
            log() << " --maxConns too high, can only handle " << want << endl;
        }
    }
    globalTicketHolder.resize(want);
}

void Listener::shutdown() {
    _finished.store(true);
}

TicketHolder Listener::globalTicketHolder(DEFAULT_MAX_CONN);
AtomicInt64 Listener::globalConnectionNumber;

void ListeningSockets::closeAll() {
    std::set<int>* sockets;
    std::set<std::string>* paths;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        sockets = _sockets;
        _sockets = new std::set<int>();
        paths = _socketPaths;
        _socketPaths = new std::set<std::string>();
    }

    for (std::set<int>::iterator i = sockets->begin(); i != sockets->end(); i++) {
        int sock = *i;
        log() << "closing listening socket: " << sock << std::endl;
        closesocket(sock);
    }
    delete sockets;

    for (std::set<std::string>::iterator i = paths->begin(); i != paths->end(); i++) {
        std::string path = *i;
        log() << "removing socket file: " << path << std::endl;
        ::remove(path.c_str());
    }
    delete paths;
}
}
