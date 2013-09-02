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

#include <stdio.h>

#ifndef _WIN32

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#ifdef __openbsd__
# include <sys/uio.h>
#endif

#endif // not _WIN32

#include <boost/scoped_ptr.hpp>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/logger/log_severity.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/cstdint.h"
#include "mongo/util/assert_util.h"

namespace mongo {

#ifdef MONGO_SSL
    class SSLManagerInterface;
    class SSLConnection;
#endif

    extern const int portSendFlags;
    extern const int portRecvFlags;

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

    std::string makeUnixSockPath(int port);

    // If an ip address is passed in, just return that.  If a hostname is passed
    // in, look up its ip and return that.  Returns "" on failure.
    std::string hostbyname(const char *hostname);

    void enableIPv6(bool state=true);
    bool IPv6Enabled();
    void setSockTimeouts(int sock, double secs);

    /**
     * wrapped around os representation of network address
     */
    struct SockAddr {
        SockAddr() {
            addressSize = sizeof(sa);
            memset(&sa, 0, sizeof(sa));
            sa.ss_family = AF_UNSPEC;
        }
        explicit SockAddr(int sourcePort); /* listener side */
        SockAddr(const char *ip, int port); /* EndPoint (remote) side, or if you want to specify which interface locally */

        template <typename T> T& as() { return *(T*)(&sa); }
        template <typename T> const T& as() const { return *(const T*)(&sa); }
        
        std::string toString(bool includePort=true) const;

        /** 
         * @return one of AF_INET, AF_INET6, or AF_UNIX
         */
        sa_family_t getType() const;

        unsigned getPort() const;

        std::string getAddr() const;

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
    std::string getHostName();
    
    /** this is cached, so if changes during the process lifetime
     * will be stale */
    std::string getHostNameCached();

    std::string prettyHostName();

    /**
     * thrown by Socket and SockAddr
     */
    class SocketException : public DBException {
    public:
        const enum Type { CLOSED , RECV_ERROR , SEND_ERROR, RECV_TIMEOUT, SEND_TIMEOUT, FAILED_STATE, CONNECT_ERROR } _type;
        
        SocketException( Type t , const std::string& server , int code = 9001 , const std::string& extra="" ) 
            : DBException( std::string("socket exception [")  + _getStringType( t ) + "] for " + server, code ),
              _type(t),
              _server(server),
              _extra(extra)
        {}

        virtual ~SocketException() throw() {}

        bool shouldPrint() const { return _type != CLOSED; }
        virtual std::string toString() const;
        virtual const std::string* server() const { return &_server; }
    private:

        // TODO: Allow exceptions better control over their messages
        static std::string _getStringType( Type t ){
            switch (t) {
                case CLOSED:        return "CLOSED";
                case RECV_ERROR:    return "RECV_ERROR";
                case SEND_ERROR:    return "SEND_ERROR";
                case RECV_TIMEOUT:  return "RECV_TIMEOUT";
                case SEND_TIMEOUT:  return "SEND_TIMEOUT";
                case FAILED_STATE:  return "FAILED_STATE";
                case CONNECT_ERROR: return "CONNECT_ERROR";
                default:            return "UNKNOWN"; // should never happen
            }
        }

        std::string _server;
        std::string _extra;
    };


    /**
     * thin wrapped around file descriptor and system calls
     * todo: ssl
     */
    class Socket {
        MONGO_DISALLOW_COPYING(Socket);
    public:

        static const int errorPollIntervalSecs;

        Socket(int sock, const SockAddr& farEnd);

        /** In some cases the timeout will actually be 2x this value - eg we do a partial send,
            then the timeout fires, then we try to send again, then the timeout fires again with
            no data sent, then we detect that the other side is down.

            Generally you don't want a timeout, you should be very prepared for errors if you set one.
        */
        Socket(double so_timeout = 0, logger::LogSeverity logLevel = logger::LogSeverity::Log() );

        ~Socket();

        bool connect(SockAddr& farEnd);
        void close();
        
        void send( const char * data , int len, const char *context );
        void send( const std::vector< std::pair< char *, int > > &data, const char *context );

        // recv len or throw SocketException
        void recv( char * data , int len );
        int unsafe_recv( char *buf, int max );
        
        logger::LogSeverity getLogLevel() const { return _logLevel; }
        void setLogLevel( logger::LogSeverity ll ) { _logLevel = ll; }

        SockAddr remoteAddr() const { return _remote; }
        std::string remoteString() const { return _remote.toString(); }
        unsigned remotePort() const { return _remote.getPort(); }

        SockAddr localAddr() const { return _local; }

        void clearCounters() { _bytesIn = 0; _bytesOut = 0; }
        long long getBytesIn() const { return _bytesIn; }
        long long getBytesOut() const { return _bytesOut; }
        int rawFD() const { return _fd; }

        void setTimeout( double secs );
        bool isStillConnected();

#ifdef MONGO_SSL
        /** secures inline */
        void secure( SSLManagerInterface* ssl );

        void secureAccepted( SSLManagerInterface* ssl );
#endif
        
        /**
         * This function calls SSL_accept() if SSL-encrypted sockets
         * are desired. SSL_accept() waits until the remote host calls
         * SSL_connect(). The return value is the subject name of any
         * client certificate provided during the handshake.
         * This function may throw SocketException.
         */
        std::string doSSLHandshake();
        
        /**
         * @return the time when the socket was opened.
         */
        uint64_t getSockCreationMicroSec() const {
            return _fdCreationMicroSec;
        }

        void handleRecvError(int ret, int len);
        MONGO_COMPILER_NORETURN void handleSendError(int ret, const char* context);

    private:
        void _init();

        /** sends dumbly, just each buffer at a time */
        void _send( const std::vector< std::pair< char *, int > > &data, const char *context );

        /** raw send, same semantics as ::send with an additional context parameter */
        int _send( const char * data , int len , const char * context );

        /** raw recv, same semantics as ::recv */
        int _recv( char * buf , int max );

        int _fd;
        uint64_t _fdCreationMicroSec;
        SockAddr _local;
        SockAddr _remote;
        double _timeout;

        long long _bytesIn;
        long long _bytesOut;
        time_t _lastValidityCheckAtSecs;

#ifdef MONGO_SSL
        boost::scoped_ptr<SSLConnection> _sslConnection;
        SSLManagerInterface* _sslManager;
#endif
        logger::LogSeverity _logLevel; // passed to log() when logging errors

    };


} // namespace mongo
