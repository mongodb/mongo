// @file sock.cpp

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

#include "mongo/util/net/sock.h"

#if !defined(_WIN32)
# include <sys/poll.h>
# include <sys/socket.h>
# include <sys/types.h>
# include <sys/un.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
# include <arpa/inet.h>
# include <errno.h>
# include <netdb.h>
# if defined(__openbsd__)
#  include <sys/uio.h>
# endif
#endif

#ifdef MONGO_SSL
#include "mongo/util/net/ssl_manager.h"
#endif

#include "mongo/util/background.h"
#include "mongo/util/concurrency/value.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/db/cmdline.h"

namespace mongo {
    MONGO_FP_DECLARE(throwSockExcep);

    static bool ipv6 = false;
    void enableIPv6(bool state) { ipv6 = state; }
    bool IPv6Enabled() { return ipv6; }
    
    void setSockTimeouts(int sock, double secs) {
        struct timeval tv;
        tv.tv_sec = (int)secs;
        tv.tv_usec = (int)((long long)(secs*1000*1000) % (1000*1000));
        bool report = logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(4));
        DEV report = true;
#if defined(_WIN32)
        tv.tv_sec *= 1000; // Windows timeout is a DWORD, in milliseconds.
        int status = setsockopt( sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &tv.tv_sec, sizeof(DWORD) ) == 0;
        if( report && (status == SOCKET_ERROR) ) log() << "unable to set SO_RCVTIMEO" << endl;
        status = setsockopt( sock, SOL_SOCKET, SO_SNDTIMEO, (char *) &tv.tv_sec, sizeof(DWORD) ) == 0;
        DEV if( report && (status == SOCKET_ERROR) ) log() << "unable to set SO_SNDTIMEO" << endl;
#else
        bool ok = setsockopt( sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &tv, sizeof(tv) ) == 0;
        if( report && !ok ) log() << "unable to set SO_RCVTIMEO" << endl;
        ok = setsockopt( sock, SOL_SOCKET, SO_SNDTIMEO, (char *) &tv, sizeof(tv) ) == 0;
        DEV if( report && !ok ) log() << "unable to set SO_SNDTIMEO" << endl;
#endif
    }

#if defined(_WIN32)
    void disableNagle(int sock) {
        int x = 1;
        if ( setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &x, sizeof(x)) )
            error() << "disableNagle failed: " << errnoWithDescription() << endl;
        if ( setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *) &x, sizeof(x)) )
            error() << "SO_KEEPALIVE failed: " << errnoWithDescription() << endl;
    }
#else
    
    void disableNagle(int sock) {
        int x = 1;

#ifdef SOL_TCP
        int level = SOL_TCP;
#else
        int level = SOL_SOCKET;
#endif

        if ( setsockopt(sock, level, TCP_NODELAY, (char *) &x, sizeof(x)) )
            error() << "disableNagle failed: " << errnoWithDescription() << endl;

#ifdef SO_KEEPALIVE
        if ( setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *) &x, sizeof(x)) )
            error() << "SO_KEEPALIVE failed: " << errnoWithDescription() << endl;

#  ifdef __linux__
        socklen_t len = sizeof(x);
        if ( getsockopt(sock, level, TCP_KEEPIDLE, (char *) &x, &len) )
            error() << "can't get TCP_KEEPIDLE: " << errnoWithDescription() << endl;

        if (x > 300) {
            x = 300;
            if ( setsockopt(sock, level, TCP_KEEPIDLE, (char *) &x, sizeof(x)) ) {
                error() << "can't set TCP_KEEPIDLE: " << errnoWithDescription() << endl;
            }
        }

        len = sizeof(x); // just in case it changed
        if ( getsockopt(sock, level, TCP_KEEPINTVL, (char *) &x, &len) )
            error() << "can't get TCP_KEEPINTVL: " << errnoWithDescription() << endl;

        if (x > 300) {
            x = 300;
            if ( setsockopt(sock, level, TCP_KEEPINTVL, (char *) &x, sizeof(x)) ) {
                error() << "can't set TCP_KEEPINTVL: " << errnoWithDescription() << endl;
            }
        }
#  endif
#endif

    }

#endif

    string getAddrInfoStrError(int code) {
#if !defined(_WIN32)
        return gai_strerror(code);
#else
        /* gai_strerrorA is not threadsafe on windows. don't use it. */
        return errnoWithDescription(code);
#endif
    }

    // --- SockAddr

    SockAddr::SockAddr(int sourcePort) {
        memset(as<sockaddr_in>().sin_zero, 0, sizeof(as<sockaddr_in>().sin_zero));
        as<sockaddr_in>().sin_family = AF_INET;
        as<sockaddr_in>().sin_port = htons(sourcePort);
        as<sockaddr_in>().sin_addr.s_addr = htonl(INADDR_ANY);
        addressSize = sizeof(sockaddr_in);
    }

    SockAddr::SockAddr(const char * _iporhost , int port) {
        string target = _iporhost;
        if( target == "localhost" ) {
            target = "127.0.0.1";
        }

        if( mongoutils::str::contains(target, '/') ) {
#ifdef _WIN32
            uassert(13080, "no unix socket support on windows", false);
#endif
            uassert(13079, "path to unix socket too long", 
                    target.size() < sizeof(as<sockaddr_un>().sun_path));
            as<sockaddr_un>().sun_family = AF_UNIX;
            strcpy(as<sockaddr_un>().sun_path, target.c_str());
            addressSize = sizeof(sockaddr_un);
        }
        else {
            addrinfo* addrs = NULL;
            addrinfo hints;
            memset(&hints, 0, sizeof(addrinfo));
            hints.ai_socktype = SOCK_STREAM;
            //hints.ai_flags = AI_ADDRCONFIG; // This is often recommended but don't do it. 
                                              // SERVER-1579
            hints.ai_flags |= AI_NUMERICHOST; // first pass tries w/o DNS lookup
            hints.ai_family = (IPv6Enabled() ? AF_UNSPEC : AF_INET);

            StringBuilder ss;
            ss << port;
            int ret = getaddrinfo(target.c_str(), ss.str().c_str(), &hints, &addrs);

            // old C compilers on IPv6-capable hosts return EAI_NODATA error
#ifdef EAI_NODATA
            int nodata = (ret == EAI_NODATA);
#else
            int nodata = false;
#endif
            if ( (ret == EAI_NONAME || nodata) ) {
                // iporhost isn't an IP address, allow DNS lookup
                hints.ai_flags &= ~AI_NUMERICHOST;
                ret = getaddrinfo(target.c_str(), ss.str().c_str(), &hints, &addrs);
            }

            if (ret) {
                // we were unsuccessful
                if( target != "0.0.0.0" ) { // don't log if this as it is a 
                                            // CRT construction and log() may not work yet.
                    log() << "getaddrinfo(\"" << target << "\") failed: " << 
                        getAddrInfoStrError(ret) << endl;
                }
                *this = SockAddr(port);
            }
            else {
                //TODO: handle other addresses in linked list;
                fassert(16501, addrs->ai_addrlen <= sizeof(sa));
                memcpy(&sa, addrs->ai_addr, addrs->ai_addrlen);
                addressSize = addrs->ai_addrlen;
                freeaddrinfo(addrs);
            }
        }
    }

    bool SockAddr::isLocalHost() const {
        switch (getType()) {
        case AF_INET: return getAddr() == "127.0.0.1";
        case AF_INET6: return getAddr() == "::1";
        case AF_UNIX: return true;
        default: return false;
        }
        fassert(16502, false);
        return false;
    }

    string SockAddr::toString(bool includePort) const {
        string out = getAddr();
        if (includePort && getType() != AF_UNIX && getType() != AF_UNSPEC)
            out += mongoutils::str::stream() << ':' << getPort();
        return out;
    }
    
    sa_family_t SockAddr::getType() const {
        return sa.ss_family;
    }
    
    unsigned SockAddr::getPort() const {
        switch (getType()) {
        case AF_INET:  return ntohs(as<sockaddr_in>().sin_port);
        case AF_INET6: return ntohs(as<sockaddr_in6>().sin6_port);
        case AF_UNIX: return 0;
        case AF_UNSPEC: return 0;
        default: massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false); return 0;
        }
    }
    
    std::string SockAddr::getAddr() const {
        switch (getType()) {
        case AF_INET:
        case AF_INET6: {
            const int buflen=128;
            char buffer[buflen];
            int ret = getnameinfo(raw(), addressSize, buffer, buflen, NULL, 0, NI_NUMERICHOST);
            massert(13082, mongoutils::str::stream() << "getnameinfo error " 
                    << getAddrInfoStrError(ret), ret == 0);
            return buffer;
        }
            
        case AF_UNIX:  
            return (addressSize > 2 ? as<sockaddr_un>().sun_path : "anonymous unix socket");
        case AF_UNSPEC: 
            return "(NONE)";
        default: 
            massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false); return "";
        }
    }

    bool SockAddr::operator==(const SockAddr& r) const {
        if (getType() != r.getType())
            return false;
        
        if (getPort() != r.getPort())
            return false;
        
        switch (getType()) {
        case AF_INET:  
            return as<sockaddr_in>().sin_addr.s_addr == r.as<sockaddr_in>().sin_addr.s_addr;
        case AF_INET6: 
            return memcmp(as<sockaddr_in6>().sin6_addr.s6_addr, 
                          r.as<sockaddr_in6>().sin6_addr.s6_addr, 
                          sizeof(in6_addr)) == 0;
        case AF_UNIX:  
            return strcmp(as<sockaddr_un>().sun_path, r.as<sockaddr_un>().sun_path) == 0;
        case AF_UNSPEC: 
            return true; // assume all unspecified addresses are the same
        default: 
            massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false);
        }
        return false;
    }
    
    bool SockAddr::operator!=(const SockAddr& r) const {
        return !(*this == r);
    }
    
    bool SockAddr::operator<(const SockAddr& r) const {
        if (getType() < r.getType())
            return true;
        else if (getType() > r.getType())
            return false;
        
        if (getPort() < r.getPort())
            return true;
        else if (getPort() > r.getPort())
            return false;
        
        switch (getType()) {
        case AF_INET:  
            return as<sockaddr_in>().sin_addr.s_addr < r.as<sockaddr_in>().sin_addr.s_addr;
        case AF_INET6: 
            return memcmp(as<sockaddr_in6>().sin6_addr.s6_addr, 
                          r.as<sockaddr_in6>().sin6_addr.s6_addr, 
                          sizeof(in6_addr)) < 0;
        case AF_UNIX:  
            return strcmp(as<sockaddr_un>().sun_path, r.as<sockaddr_un>().sun_path) < 0;
        case AF_UNSPEC: 
            return false;
        default: 
            massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false);
        }
        return false;        
    }

    SockAddr unknownAddress( "0.0.0.0", 0 );

    string makeUnixSockPath(int port) {
        return mongoutils::str::stream() << cmdLine.socket << "/mongodb-" << port << ".sock";
    }


    // If an ip address is passed in, just return that.  If a hostname is passed
    // in, look up its ip and return that.  Returns "" on failure.
    string hostbyname(const char *hostname) {
        string addr =  SockAddr(hostname, 0).getAddr();
        if (addr == "0.0.0.0")
            return "";
        else
            return addr;
    }
   
    //  --- my --

    DiagStr& _hostNameCached = *(new DiagStr); // this is also written to from commands/cloud.cpp

    string getHostName() {
        char buf[256];
        int ec = gethostname(buf, 127);
        if ( ec || *buf == 0 ) {
            log() << "can't get this server's hostname " << errnoWithDescription() << endl;
            return "";
        }
        return buf;
    }

    /** we store our host name once */
    string getHostNameCached() {
        string temp = _hostNameCached.get();
        if (_hostNameCached.empty()) {
            temp = getHostName();
            _hostNameCached = temp;
        }
        return temp;
    }

    string prettyHostName() {
        StringBuilder s;
        s << getHostNameCached();
        if( cmdLine.port != CmdLine::DefaultDBPort )
            s << ':' << mongo::cmdLine.port;
        return s.str();
    }

    // --------- SocketException ----------

#ifdef MSG_NOSIGNAL
    const int portSendFlags = MSG_NOSIGNAL;
    const int portRecvFlags = MSG_NOSIGNAL;
#else
    const int portSendFlags = 0;
    const int portRecvFlags = 0;
#endif

    string SocketException::toString() const {
        stringstream ss;
        ss << _ei.code << " socket exception [" << _getStringType(_type) << "] ";
        
        if ( _server.size() )
            ss << "server [" << _server << "] ";
        
        if ( _extra.size() )
            ss << _extra;
        
        return ss.str();
    }

    // ------------ Socket -----------------

    static int socketGetLastError() {
#ifdef _WIN32
        return WSAGetLastError();
#else
        return errno;
#endif
    }

    static SockAddr getLocalAddrForBoundSocketFd(int fd) {
        SockAddr result;
        int rc = getsockname(fd, result.raw(), &result.addressSize);
        if (rc != 0) {
            warning() << "Could not resolve local address for socket with fd " << fd << ": " <<
                getAddrInfoStrError(socketGetLastError());
            result = SockAddr();
        }
        return result;
    }

    Socket::Socket(int fd , const SockAddr& remote) : 
        _fd(fd), _remote(remote), _timeout(0), _lastValidityCheckAtSecs(time(0)), _logLevel(logger::LogSeverity::Log()) {
        _init();
        if (fd >= 0) {
            _local = getLocalAddrForBoundSocketFd(_fd);
        }
    }

    Socket::Socket( double timeout, logger::LogSeverity ll ) : _logLevel(ll) {
        _fd = -1;
        _timeout = timeout;
        _lastValidityCheckAtSecs = time(0);
        _init();
    }

    Socket::~Socket() {
        close();
#ifdef MONGO_SSL
        if (_sslConnection.get()) {
            _sslManager->SSL_shutdown( _sslConnection.get() );
        }
#endif
    }
    
    void Socket::_init() {
        _bytesOut = 0;
        _bytesIn = 0;
#ifdef MONGO_SSL
        _sslManager = 0;
#endif
    }

    void Socket::close() {
        if ( _fd >= 0 ) {
            // Stop any blocking reads/writes, and prevent new reads/writes
#if defined(_WIN32)
            shutdown( _fd, SD_BOTH );
#else
            shutdown( _fd, SHUT_RDWR );
#endif
            closesocket( _fd );
            _fd = -1;
        }
    }

#ifdef MONGO_SSL
    void Socket::secure(SSLManagerInterface* mgr) {
        fassert(16503, mgr);
        fassert(16505, _fd >= 0);
        _sslManager = mgr;
        _sslConnection.reset(_sslManager->connect(this));
        mgr->validatePeerCertificate(_sslConnection.get());
    }

    void Socket::secureAccepted( SSLManagerInterface* ssl ) { 
        _sslManager = ssl;
    }

    std::string Socket::doSSLHandshake() {
        if (!_sslManager) return "";
        fassert(16506, _fd);
        _sslConnection.reset(_sslManager->accept(this));
        return _sslManager->validatePeerCertificate(_sslConnection.get());
    }
#endif

    class ConnectBG : public BackgroundJob {
    public:
        ConnectBG(int sock, SockAddr remote) : _sock(sock), _remote(remote) { }

        void run() { _res = ::connect(_sock, _remote.raw(), _remote.addressSize); }
        string name() const { return "ConnectBG"; }
        int inError() const { return _res; }

    private:
        int _sock;
        int _res;
        SockAddr _remote;
    };

    bool Socket::connect(SockAddr& remote) {
        _remote = remote;

        _fd = socket(remote.getType(), SOCK_STREAM, 0);
        if ( _fd == INVALID_SOCKET ) {
            LOG(_logLevel) << "ERROR: connect invalid socket " << errnoWithDescription() << endl;
            return false;
        }

        if ( _timeout > 0 ) {
            setTimeout( _timeout );
        }

        ConnectBG bg(_fd, remote);
        bg.go();
        if ( bg.wait(5000) ) {
            if ( bg.inError() ) {
                close();
                return false;
            }
        }
        else {
            // time out the connect
            close();
            bg.wait(); // so bg stays in scope until bg thread terminates
            return false;
        }

        if (remote.getType() != AF_UNIX)
            disableNagle(_fd);

#ifdef SO_NOSIGPIPE
        // ignore SIGPIPE signals on osx, to avoid process exit
        const int one = 1;
        setsockopt( _fd , SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(int));
#endif

        _local = getLocalAddrForBoundSocketFd(_fd);

        _fdCreationMicroSec = curTimeMicros64();
        return true;
    }

    // throws if SSL_write or send fails 
    int Socket::_send( const char * data , int len, const char * context ) {
#ifdef MONGO_SSL
        if ( _sslConnection.get() ) {
            return _sslManager->SSL_write( _sslConnection.get() , data , len );
        }
#endif
        int ret = ::send( _fd , data , len , portSendFlags );
        if (ret < 0) {
            handleSendError(ret, context);
        }
        return ret;
    }

    // sends all data or throws an exception
    void Socket::send( const char * data , int len, const char *context ) {
        while( len > 0 ) {
            int ret = -1;
            if (MONGO_FAIL_POINT(throwSockExcep)) {
#if defined(_WIN32)
                WSASetLastError(WSAENETUNREACH);
#else
                errno = ENETUNREACH;
#endif
                handleSendError(ret, context);
            }
            else {
                ret = _send(data, len, context);
            }

            _bytesOut += ret;

            fassert(16507, ret <= len);
            len -= ret;
            data += ret;

        }
    }

    void Socket::_send( const vector< pair< char *, int > > &data, const char *context ) {
        for (vector< pair<char *, int> >::const_iterator i = data.begin(); 
             i != data.end(); 
             ++i) {
            char * data = i->first;
            int len = i->second;
            send( data, len, context );
        }
    }

    /** sends all data or throws an exception
     * @param context descriptive for logging
     */
    void Socket::send( const vector< pair< char *, int > > &data, const char *context ) {

#ifdef MONGO_SSL
        if ( _sslConnection.get() ) {
            _send( data , context );
            return;
        }
#endif

#if defined(_WIN32)
        // TODO use scatter/gather api
        _send( data , context );
#else
        vector<struct iovec> d( data.size() );
        int i = 0;
        for (vector< pair<char *, int> >::const_iterator j = data.begin(); 
             j != data.end(); 
             ++j) {
            if ( j->second > 0 ) {
                d[ i ].iov_base = j->first;
                d[ i ].iov_len = j->second;
                ++i;
                _bytesOut += j->second;
            }
        }
        struct msghdr meta;
        memset( &meta, 0, sizeof( meta ) );
        meta.msg_iov = &d[ 0 ];
        meta.msg_iovlen = d.size();

        while( meta.msg_iovlen > 0 ) {
            int ret = -1;
            if (MONGO_FAIL_POINT(throwSockExcep)) {
#if defined(_WIN32)
                WSASetLastError(WSAENETUNREACH);
#else
                errno = ENETUNREACH;
#endif
            }
            else {
                ret = ::sendmsg(_fd, &meta, portSendFlags);
            }

            if (ret == -1) {
                if ( errno != EAGAIN || _timeout == 0 ) {
                    LOG(_logLevel) << "Socket " << context << 
                        " send() " << errnoWithDescription() << ' ' << remoteString() << endl;
                    throw SocketException( SocketException::SEND_ERROR , remoteString() );
                }
                else {
                    LOG(_logLevel) << "Socket " << context << 
                        " send() remote timeout " << remoteString() << endl;
                    throw SocketException( SocketException::SEND_TIMEOUT , remoteString() );
                }
            }
            else {
                struct iovec *& i = meta.msg_iov;
                while( ret > 0 ) {
                    if ( i->iov_len > unsigned( ret ) ) {
                        i->iov_len -= ret;
                        i->iov_base = (char*)(i->iov_base) + ret;
                        ret = 0;
                    }
                    else {
                        ret -= i->iov_len;
                        ++i;
                        --(meta.msg_iovlen);
                    }
                }
            }
        }
#endif
    }

    void Socket::recv( char * buf , int len ) {
        while( len > 0 ) {
            int ret = -1;
            if (MONGO_FAIL_POINT(throwSockExcep)) {
#if defined(_WIN32)
                WSASetLastError(WSAENETUNREACH);
#else
                errno = ENETUNREACH;
#endif
                if (ret <= 0) {
                    handleRecvError(ret, len);
                    continue;
                }
            }
            else {
                ret = unsafe_recv(buf, len);
            }

            if ( len <= 4 && ret != len ) {
                LOG(_logLevel) << "Socket recv() got " << ret <<
                    " bytes wanted len=" << len << endl;
            }
            fassert(16508, ret <= len);
            len -= ret;
            buf += ret;
        }
    }

    int Socket::unsafe_recv( char *buf, int max ) {
        int x = _recv( buf , max );
        _bytesIn += x;
        return x;
    }

    // throws if SSL_read fails or recv returns an error
    int Socket::_recv( char *buf, int max ) {
#ifdef MONGO_SSL
        if ( _sslConnection.get() ){
            return _sslManager->SSL_read( _sslConnection.get() , buf , max );
        }
#endif
        int ret = ::recv( _fd , buf , max , portRecvFlags );
        if (ret <= 0) {
            handleRecvError(ret, max); // If no throw return and call _recv again
            return 0;
        }
        return ret;
    }

    void Socket::handleSendError(int ret, const char* context) {

#if defined(_WIN32)
        const int mongo_errno = WSAGetLastError();
        if ( mongo_errno == WSAETIMEDOUT && _timeout != 0 ) {
#else
        const int mongo_errno = errno;
        if ( ( mongo_errno == EAGAIN || mongo_errno == EWOULDBLOCK ) && _timeout != 0 ) {
#endif
            LOG(_logLevel) << "Socket " << context << 
                " send() timed out " << remoteString() << endl;
            throw SocketException(SocketException::SEND_TIMEOUT , remoteString());
        }
        else {
            LOG(_logLevel) << "Socket " << context << " send() "
                           << errnoWithDescription(mongo_errno) << ' ' << remoteString() << endl;
            throw SocketException(SocketException::SEND_ERROR , remoteString());            
        }
    }

    void Socket::handleRecvError(int ret, int len) {
        if (ret == 0) {
            LOG(3) << "Socket recv() conn closed? " << remoteString() << endl;
            throw SocketException(SocketException::CLOSED , remoteString());
        }
     
        // ret < 0
#if defined(_WIN32)
        int e = WSAGetLastError();
#else
        int e = errno;
# if defined(EINTR)
        if (e == EINTR) {
            LOG(_logLevel) << "EINTR returned from recv(), retrying";
            return;
        }
# endif
#endif

#if defined(_WIN32)
        // Windows
        if ((e == EAGAIN || e == WSAETIMEDOUT) && _timeout > 0) { 
#else
        if (e == EAGAIN && _timeout > 0) { 
#endif
            // this is a timeout
            LOG(_logLevel) << "Socket recv() timeout  " << remoteString() <<endl;
            throw SocketException(SocketException::RECV_TIMEOUT, remoteString());
        }

        LOG(_logLevel) << "Socket recv() " << 
            errnoWithDescription(e) << " " << remoteString() <<endl;
        throw SocketException(SocketException::RECV_ERROR , remoteString());
    }

    void Socket::setTimeout( double secs ) {
        setSockTimeouts( _fd, secs );
    }

    // TODO: allow modification?
    //
    // <positive value> : secs to wait between stillConnected checks
    // 0 : always check
    // -1 : never check
    const int Socket::errorPollIntervalSecs( 5 );

#if defined(NTDDI_VERSION) && ( !defined(NTDDI_VISTA) || ( NTDDI_VERSION < NTDDI_VISTA ) )
    // Windows XP

    // pre-Vista windows doesn't have WSAPoll, so don't test connections
    bool Socket::isStillConnected() {
        return true;
    }

#else // Not Windows XP

    // Patch to allow better tolerance of flaky network connections that get broken
    // while we aren't looking.
    // TODO: Remove when better async changes come.
    //
    // isStillConnected() polls the socket at max every Socket::errorPollIntervalSecs to determine
    // if any disconnection-type events have happened on the socket.
    bool Socket::isStillConnected() {

        if ( errorPollIntervalSecs < 0 ) return true;

        time_t now = time( 0 );
        time_t idleTimeSecs = now - _lastValidityCheckAtSecs;

        // Only check once every 5 secs
        if ( idleTimeSecs < errorPollIntervalSecs ) return true;
        // Reset our timer, we're checking the connection
        _lastValidityCheckAtSecs = now;

        // It's been long enough, poll to see if our socket is still connected

        pollfd pollInfo;
        pollInfo.fd = _fd;
        // We only care about reading the EOF message on clean close (and errors)
        pollInfo.events = POLLIN;

        // Poll( info[], size, timeout ) - timeout == 0 => nonblocking
#if defined(_WIN32)
        int nEvents = WSAPoll( &pollInfo, 1, 0 );
#else
        int nEvents = ::poll( &pollInfo, 1, 0 );
#endif

        LOG( 2 ) << "polling for status of connection to " << remoteString()
                 << ", " << ( nEvents == 0 ? "no events" :
                              nEvents == -1 ? "error detected" :
                                               "event detected" ) << endl;

        if ( nEvents == 0 ) {
            // No events incoming, return still connected AFAWK
            return true;
        }
        else if ( nEvents < 0 ) {
            // Poll itself failed, this is weird, warn and log errno
            warning() << "Socket poll() failed during connectivity check"
                      << " (idle " << idleTimeSecs << " secs,"
                      << " remote host " << remoteString() << ")"
                      << causedBy(errnoWithDescription()) << endl;

            // Return true since it's not clear that we're disconnected.
            return true;
        }

        dassert( nEvents == 1 );
        dassert( pollInfo.revents > 0 );

        // Return false at this point, some event happened on the socket, but log what the
        // actual event was.

        if ( pollInfo.revents & POLLIN ) {

            // There shouldn't really be any data to recv here, so make sure this
            // is a clean hangup.

            // Used concurrently, but we never actually read this data
            static char testBuf[1];

            int recvd = ::recv( _fd, testBuf, 1, portRecvFlags );

            if ( recvd < 0 ) {
                // An error occurred during recv, warn and log errno
                warning() << "Socket recv() failed during connectivity check"
                          << " (idle " << idleTimeSecs << " secs,"
                          << " remote host " << remoteString() << ")"
                          << causedBy(errnoWithDescription()) << endl;
            }
            else if ( recvd > 0 ) {
                // We got nonzero data from this socket, very weird?
                // Log and warn at runtime, log and abort at devtime
                // TODO: Dump the data to the log somehow?
                error() << "Socket found pending data during connectivity check"
                        << " (idle " << idleTimeSecs << " secs,"
                        << " remote host " << remoteString() << ")" << endl;
                dassert( false );
            }
            else {
                // recvd == 0, socket closed remotely, just return false
                LOG( 0 ) << "Socket closed remotely, no longer connected"
                         << " (idle " << idleTimeSecs << " secs,"
                         << " remote host " << remoteString() << ")" << endl;
            }
        }
        else if ( pollInfo.revents & POLLHUP ) {
            // A hangup has occurred on this socket
            LOG( 0 ) << "Socket hangup detected, no longer connected" << " (idle "
                         << idleTimeSecs << " secs," << " remote host " << remoteString() << ")"
                         << endl;
        }
        else if ( pollInfo.revents & POLLERR ) {
            // An error has occurred on this socket
            LOG( 0 ) << "Socket error detected, no longer connected" << " (idle "
                         << idleTimeSecs << " secs," << " remote host " << remoteString() << ")"
                         << endl;
        }
        else if ( pollInfo.revents & POLLNVAL ) {
            // Socket descriptor itself is weird
            // Log and warn at runtime, log and abort at devtime
            error() << "Socket descriptor detected as invalid"
                    << " (idle " << idleTimeSecs << " secs,"
                    << " remote host " << remoteString() << ")" << endl;
            dassert( false );
        }
        else {
            // Don't know what poll is saying here
            // Log and warn at runtime, log and abort at devtime
            error() << "Socket had unknown event (" << static_cast<int>(pollInfo.revents) << ")"
                    << " (idle " << idleTimeSecs << " secs,"
                    << " remote host " << remoteString() << ")" << endl;
            dassert( false );
        }

        return false;
    }

#endif // End Not Windows XP

#if defined(_WIN32)
    struct WinsockInit {
        WinsockInit() {
            WSADATA d;
            if ( WSAStartup(MAKEWORD(2,2), &d) != 0 ) {
                out() << "ERROR: wsastartup failed " << errnoWithDescription() << endl;
                problem() << "ERROR: wsastartup failed " << errnoWithDescription() << endl;
                _exit(EXIT_NTSERVICE_ERROR);
            }
        }
    } winsock_init;
#endif

} // namespace mongo
