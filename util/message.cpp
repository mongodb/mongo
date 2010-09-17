/* message

   todo: authenticate; encrypt?
*/

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
#include "message.h"
#include <time.h>
#include "../util/goodies.h"
#include "../util/background.h"
#include <fcntl.h>
#include <errno.h>
#include "../db/cmdline.h"
#include "../client/dbclient.h"
#include "../util/time_support.h"

#ifndef _WIN32
# ifndef __sunos__
#  include <ifaddrs.h>
# endif
# include <sys/resource.h>
# include <sys/stat.h>
#else

// errno doesn't work for winsock.
#undef errno
#define errno WSAGetLastError()

#endif

namespace mongo {

    bool noUnixSocket = false;

    bool objcheck = false;

    void checkTicketNumbers();
    
// if you want trace output:
#define mmm(x)

#ifdef MSG_NOSIGNAL
    const int portSendFlags = MSG_NOSIGNAL;
    const int portRecvFlags = MSG_NOSIGNAL;
#else
    const int portSendFlags = 0;
    const int portRecvFlags = 0;
#endif

    const Listener* Listener::_timeTracker;

    vector<SockAddr> ipToAddrs(const char* ips, int port){
        vector<SockAddr> out;
        if (*ips == '\0'){
            out.push_back(SockAddr("0.0.0.0", port)); // IPv4 all

            if (IPv6Enabled())
                out.push_back(SockAddr("::", port)); // IPv6 all
#ifndef _WIN32
            if (!noUnixSocket)
                out.push_back(SockAddr(makeUnixSockPath(port).c_str(), port)); // Unix socket
#endif
            return out;
        }

        while(*ips){
            string ip;
            const char * comma = strchr(ips, ',');
            if (comma){
                ip = string(ips, comma - ips);
                ips = comma + 1;
            }else{
                ip = string(ips);
                ips = "";
            }

            SockAddr sa(ip.c_str(), port);
            out.push_back(sa);

#ifndef _WIN32
            if (!noUnixSocket && (sa.getAddr() == "127.0.0.1" || sa.getAddr() == "0.0.0.0")) // only IPv4
                out.push_back(SockAddr(makeUnixSockPath(port).c_str(), port));
#endif
        }
        return out;

    }

    /* listener ------------------------------------------------------------------- */

    void Listener::initAndListen() {
        checkTicketNumbers();
        vector<SockAddr> mine = ipToAddrs(_ip.c_str(), _port);
        vector<int> socks;
        SOCKET maxfd = 0; // needed for select()

        for (vector<SockAddr>::iterator it=mine.begin(), end=mine.end(); it != end; ++it){
            SockAddr& me = *it;

            SOCKET sock = ::socket(me.getType(), SOCK_STREAM, 0);
            if ( sock == INVALID_SOCKET ) {
                log() << "ERROR: listen(): invalid socket? " << errnoWithDescription() << endl;
            }

            if (me.getType() == AF_UNIX){
#if !defined(_WIN32)
                if (unlink(me.getAddr().c_str()) == -1){
                    int x = errno;
                    if (x != ENOENT){
                        log() << "couldn't unlink socket file " << me << errnoWithDescription(x) << " skipping" << endl;
                        continue;
                    }
                }
#endif
            } else if (me.getType() == AF_INET6) {
                // IPv6 can also accept IPv4 connections as mapped addresses (::ffff:127.0.0.1)
                // That causes a conflict if we don't do set it to IPV6_ONLY
                const int one = 1;
                setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char*) &one, sizeof(one));
            }

            prebindOptions( sock );
            
            if ( ::bind(sock, me.raw(), me.addressSize) != 0 ) {
                int x = errno;
                log() << "listen(): bind() failed " << errnoWithDescription(x) << " for socket: " << me.toString() << endl;
                if ( x == EADDRINUSE )
                    log() << "  addr already in use" << endl;
                closesocket(sock);
                return;
            }

#if !defined(_WIN32)
            if (me.getType() == AF_UNIX){
                if (chmod(me.getAddr().c_str(), 0777) == -1){
                    log() << "couldn't chmod socket file " << me << errnoWithDescription() << endl;
                }
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

            for (vector<int>::iterator it=socks.begin(), end=socks.end(); it != end; ++it){
                FD_SET(*it, fds);
            }

            maxSelectTime.tv_sec = 0;
            maxSelectTime.tv_usec = 10000;
            const int ret = select(maxfd+1, fds, NULL, NULL, &maxSelectTime);
            
            if (ret == 0){
#if defined(__linux__)
                _elapsedTime += ( 10000 - maxSelectTime.tv_usec ) / 1000;
#else
                _elapsedTime += 10;
#endif
                continue;
            }
            _elapsedTime += ret; // assume 1ms to grab connection. very rough
            
            if (ret < 0){
                int x = errno;
#ifdef EINTR
                if ( x == EINTR ){
                    log() << "select() signal caught, continuing" << endl;
                    continue;
                }
#endif
                if ( ! inShutdown() )
                    log() << "select() failure: ret=" << ret << " " << errnoWithDescription(x) << endl;
                return;
            }

            for (vector<int>::iterator it=socks.begin(), end=socks.end(); it != end; ++it){
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

    void Listener::accepted(int sock, const SockAddr& from){
        accepted( new MessagingPort(sock, from) );
    }

    /* messagingport -------------------------------------------------------------- */

    class PiggyBackData {
    public:
        PiggyBackData( MessagingPort * port ) {
            _port = port;
            _buf = new char[1300];
            _cur = _buf;
        }

        ~PiggyBackData() {
            DESTRUCTOR_GUARD (
                flush();
                delete[]( _cur );
            );
        }

        void append( Message& m ) {
            assert( m.header()->len <= 1300 );

            if ( len() + m.header()->len > 1300 )
                flush();

            memcpy( _cur , m.singleData() , m.header()->len );
            _cur += m.header()->len;
        }

        void flush() {
            if ( _buf == _cur )
                return;

            _port->send( _buf , len(), "flush" );
            _cur = _buf;
        }

        int len() const { return _cur - _buf; }

    private:
        MessagingPort* _port;
        char * _buf;
        char * _cur;
    };

    class Ports { 
        set<MessagingPort*> ports;
        mongo::mutex m;
    public:
        Ports() : ports(), m("Ports") {}
        void closeAll(unsigned skip_mask) {
            scoped_lock bl(m);
            for ( set<MessagingPort*>::iterator i = ports.begin(); i != ports.end(); i++ ) {
                if( (*i)->tag & skip_mask )
                    continue;
                (*i)->shutdown();
            }
        }
        void insert(MessagingPort* p) { 
            scoped_lock bl(m);
            ports.insert(p);
        }
        void erase(MessagingPort* p) { 
            scoped_lock bl(m);
            ports.erase(p);
        }
    };

    // we "new" this so it is still be around when other automatic global vars
    // are being destructed during termination.
    Ports& ports = *(new Ports());

    void MessagingPort::closeAllSockets(unsigned mask) {
        ports.closeAll(mask);
    }

    MessagingPort::MessagingPort(int _sock, const SockAddr& _far) : sock(_sock), piggyBackData(0), farEnd(_far), _timeout(), tag(0) {
        _logLevel = 0;
        ports.insert(this);
    }

    MessagingPort::MessagingPort( double timeout, int ll ) : tag(0) {
        _logLevel = ll;
        ports.insert(this);
        sock = -1;
        piggyBackData = 0;
        _timeout = timeout;
    }

    void MessagingPort::shutdown() {
        if ( sock >= 0 ) {
            closesocket(sock);
            sock = -1;
        }
    }

    MessagingPort::~MessagingPort() {
        if ( piggyBackData )
            delete( piggyBackData );
        shutdown();
        ports.erase(this);
    }

    class ConnectBG : public BackgroundJob {
    public:
        int sock;
        int res;
        SockAddr farEnd;
        ConnectBG() { nameThread = false; }
        void run() {
            res = ::connect(sock, farEnd.raw(), farEnd.addressSize);
        }
        string name() { return "ConnectBG"; }
    };

    bool MessagingPort::connect(SockAddr& _far)
    {
        farEnd = _far;

        sock = socket(farEnd.getType(), SOCK_STREAM, 0);
        if ( sock == INVALID_SOCKET ) {
            log(_logLevel) << "ERROR: connect invalid socket " << errnoWithDescription() << endl;
            return false;
        }

        if ( _timeout > 0 ) {
            setSockTimeouts( sock, _timeout );
        }
                
        ConnectBG bg;
        bg.sock = sock;
        bg.farEnd = farEnd;
        bg.go();

        if ( bg.wait(5000) ) {
            if ( bg.res ) {
                closesocket(sock);
                sock = -1;
                return false;
            }
        }
        else {
            // time out the connect
            closesocket(sock);
            sock = -1;
            bg.wait(); // so bg stays in scope until bg thread terminates
            return false;
        }

        if (farEnd.getType() != AF_UNIX)
            disableNagle(sock);

#ifdef SO_NOSIGPIPE
        // osx
        const int one = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(int));
#endif

        /*
          // SO_LINGER is bad
#ifdef SO_LINGER
        struct linger ling;
        ling.l_onoff = 1;
        ling.l_linger = 0;
        setsockopt(sock, SOL_SOCKET, SO_LINGER, (char *) &ling, sizeof(ling));
#endif
        */
        return true;
    }

    bool MessagingPort::recv(Message& m) {
        try {
        again:
            mmm( log() << "*  recv() sock:" << this->sock << endl; )
            int len = -1;
            
            char *lenbuf = (char *) &len;
            int lft = 4;
            recv( lenbuf, lft );
            
            if ( len < 16 || len > 48000000 ) { // messages must be large enough for headers
                if ( len == -1 ) {
                    // Endian check from the client, after connecting, to see what mode server is running in.
                    unsigned foo = 0x10203040;
                    send( (char *) &foo, 4, "endian" );
                    goto again;
                }
                
                if ( len == 542393671 ){
                    // an http GET
                    log(_logLevel) << "looks like you're trying to access db over http on native driver port.  please add 1000 for webserver" << endl;
                    string msg = "You are trying to access MongoDB on the native driver port. For http diagnostic access, add 1000 to the port number\n";
                    stringstream ss;
                    ss << "HTTP/1.0 200 OK\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: " << msg.size() << "\r\n\r\n" << msg;
                    string s = ss.str();
                    send( s.c_str(), s.size(), "http" );
                    return false;
                }
                log(0) << "recv(): message len " << len << " is too large" << len << endl;
                return false;
            }
            
            int z = (len+1023)&0xfffffc00;
            assert(z>=len);
            MsgData *md = (MsgData *) malloc(z);
            assert(md);
            md->len = len;
            
            char *p = (char *) &md->id;
            int left = len -4;

            try {
                recv( p, left );
            } catch (...) {
                free(md);
                throw;
            }
            
            m.setData(md, true);
            return true;
            
        } catch ( const SocketException & e ) {
            log(_logLevel + (e.shouldPrint() ? 0 : 1) ) << "SocketException: " << e << endl;
            m.reset();
            return false;
        }
    }
    
    void MessagingPort::reply(Message& received, Message& response) {
        say(/*received.from, */response, received.header()->id);
    }

    void MessagingPort::reply(Message& received, Message& response, MSGID responseTo) {
        say(/*received.from, */response, responseTo);
    }

    bool MessagingPort::call(Message& toSend, Message& response) {
        mmm( log() << "*call()" << endl; )
        MSGID old = toSend.header()->id;
        say(/*to,*/ toSend);
        while ( 1 ) {
            bool ok = recv(response);
            if ( !ok )
                return false;
            //log() << "got response: " << response.data->responseTo << endl;
            if ( response.header()->responseTo == toSend.header()->id )
                break;
            log() << "********************" << endl;
            log() << "ERROR: MessagingPort::call() wrong id got:" << hex << (unsigned)response.header()->responseTo << " expect:" << (unsigned)toSend.header()->id << endl;
            log() << "  toSend op: " << toSend.operation() << " old id:" << (unsigned)old << endl;
            log() << "  response msgid:" << (unsigned)response.header()->id << endl;
            log() << "  response len:  " << (unsigned)response.header()->len << endl;
            log() << "  response op:  " << response.operation() << endl;
            log() << "  farEnd: " << farEnd << endl;
            assert(false);
            response.reset();
        }
        mmm( log() << "*call() end" << endl; )
        return true;
    }

    void MessagingPort::say(Message& toSend, int responseTo) {
        assert( !toSend.empty() );
        mmm( log() << "*  say() sock:" << this->sock << " thr:" << GetCurrentThreadId() << endl; )
        toSend.header()->id = nextMessageId();
        toSend.header()->responseTo = responseTo;

        if ( piggyBackData && piggyBackData->len() ) {
            mmm( log() << "*     have piggy back" << endl; )
            if ( ( piggyBackData->len() + toSend.header()->len ) > 1300 ) {
                // won't fit in a packet - so just send it off
                piggyBackData->flush();
            }
            else {
                piggyBackData->append( toSend );
                piggyBackData->flush();
                return;
            }
        }

        toSend.send( *this, "say" );
    }

    // sends all data or throws an exception    
    void MessagingPort::send( const char * data , int len, const char *context ) {
        while( len > 0 ) {
            int ret = ::send( sock , data , len , portSendFlags );
            if ( ret == -1 ) {
                if ( errno != EAGAIN || _timeout == 0 ) {
                    log(_logLevel) << "MessagingPort " << context << " send() " << errnoWithDescription() << ' ' << farEnd.toString() << endl;
                    throw SocketException( SocketException::SEND_ERROR );                    
                } else {
                    if ( !serverAlive( farEnd.toString() ) ) {
                        log(_logLevel) << "MessagingPort " << context << " send() remote dead " << farEnd.toString() << endl;
                        throw SocketException( SocketException::SEND_ERROR );                        
                    }
                }
            } else {
                assert( ret <= len );
                len -= ret;
                data += ret;
            }
        }        
    }
    
    // sends all data or throws an exception
    void MessagingPort::send( const vector< pair< char *, int > > &data, const char *context ){
#if defined(_WIN32)
        // TODO use scatter/gather api
        for( vector< pair< char *, int > >::const_iterator i = data.begin(); i != data.end(); ++i ) {
            char * data = i->first;
            int len = i->second;
            send( data, len, context );
        }
#else
        vector< struct iovec > d( data.size() );
        int i = 0;
        for( vector< pair< char *, int > >::const_iterator j = data.begin(); j != data.end(); ++j ) {
            if ( j->second > 0 ) {
                d[ i ].iov_base = j->first;
                d[ i ].iov_len = j->second;
                ++i;
            }
        }
        struct msghdr meta;
        memset( &meta, 0, sizeof( meta ) );
        meta.msg_iov = &d[ 0 ];
        meta.msg_iovlen = d.size();
    
        while( meta.msg_iovlen > 0 ) {
            int ret = ::sendmsg( sock , &meta , portSendFlags );
            if ( ret == -1 ) {
                if ( errno != EAGAIN || _timeout == 0 ) {
                    log(_logLevel) << "MessagingPort " << context << " send() " << errnoWithDescription() << ' ' << farEnd.toString() << endl;
                    throw SocketException( SocketException::SEND_ERROR );                    
                } else {
                    if ( !serverAlive( farEnd.toString() ) ) {
                        log(_logLevel) << "MessagingPort " << context << " send() remote dead " << farEnd.toString() << endl;
                        throw SocketException( SocketException::SEND_ERROR );                        
                    }
                }
            } else {
                struct iovec *& i = meta.msg_iov;
                while( ret > 0 ) {
                    if ( i->iov_len > unsigned( ret ) ) {
                        i->iov_len -= ret;
                        i->iov_base = (char*)(i->iov_base) + ret;
                        ret = 0;
                    } else {
                        ret -= i->iov_len;
                        ++i;
                        --(meta.msg_iovlen);
                    }
                }
            }
        }
#endif
    }

    void MessagingPort::recv( char * buf , int len ){
        unsigned retries = 0;
        while( len > 0 ) {
            int ret = ::recv( sock , buf , len , portRecvFlags );
            if ( ret == 0 ) {
                log(3) << "MessagingPort recv() conn closed? " << farEnd.toString() << endl;
                throw SocketException( SocketException::CLOSED );
            }
            if ( ret < 0 ) {
                int e = errno;
#if defined(EINTR) && !defined(_WIN32)
                if( e == EINTR ) {
                    if( ++retries == 1 ) {
                        log() << "EINTR retry" << endl;
                        continue;
                    }
                }
#endif
                if ( e != EAGAIN || _timeout == 0 ) {                
                    log(_logLevel) << "MessagingPort recv() " << errnoWithDescription(e) << " " << farEnd.toString() <<endl;
                    throw SocketException( SocketException::RECV_ERROR );
                } else {
                    if ( !serverAlive( farEnd.toString() ) ) {
                        log(_logLevel) << "MessagingPort recv() remote dead " << farEnd.toString() << endl;
                        throw SocketException( SocketException::RECV_ERROR );                        
                    }
                }
            } else {
                if ( len <= 4 && ret != len )
                    log(_logLevel) << "MessagingPort recv() got " << ret << " bytes wanted len=" << len << endl;
                assert( ret <= len );
                len -= ret;
                buf += ret;
            }
        }
    }

    int MessagingPort::unsafe_recv( char *buf, int max ) {
        return ::recv( sock , buf , max , portRecvFlags );        
    }
    
    void MessagingPort::piggyBack( Message& toSend , int responseTo ) {

        if ( toSend.header()->len > 1300 ) {
            // not worth saving because its almost an entire packet
            say( toSend );
            return;
        }

        // we're going to be storing this, so need to set it up
        toSend.header()->id = nextMessageId();
        toSend.header()->responseTo = responseTo;

        if ( ! piggyBackData )
            piggyBackData = new PiggyBackData( this );

        piggyBackData->append( toSend );
    }

    unsigned MessagingPort::remotePort() const {
        return farEnd.getPort();
    }

    HostAndPort MessagingPort::remote() const {
        return farEnd;
    }


    MSGID NextMsgId;
    ThreadLocalValue<int> clientId;

    struct MsgStart {
        MsgStart() {
            NextMsgId = (((unsigned) time(0)) << 16) ^ curTimeMillis();
            assert(MsgDataHeaderSize == 16);
        }
    } msgstart;
    
    MSGID nextMessageId(){
        MSGID msgid = NextMsgId++;
        return msgid;
    }

    bool doesOpGetAResponse( int op ){
        return op == dbQuery || op == dbGetMore;
    }
    
    void setClientId( int id ){
        clientId.set( id );
    }
    
    int getClientId(){
        return clientId.get();
    }
    
    int getMaxConnections(){
#ifdef _WIN32
        return 20000;
#else
        struct rlimit limit;
        assert( getrlimit(RLIMIT_NOFILE,&limit) == 0 );

        int max = (int)(limit.rlim_cur * .8);

        log(1) << "fd limit" 
               << " hard:" << limit.rlim_max 
               << " soft:" << limit.rlim_cur 
               << " max conn: " << max
               << endl;
        
        if ( max > 20000 )
            max = 20000;

        return max;
#endif
    }

    void checkTicketNumbers(){
        connTicketHolder.resize( getMaxConnections() );
    }

    TicketHolder connTicketHolder(20000);

    namespace {
        map<string, bool> isSelfCache; // host, isSelf

#if !defined(_WIN32) && !defined(__sunos__)

        vector<string> getMyAddrs(){
            ifaddrs * addrs;

            int status = getifaddrs(&addrs);
            massert(13469, "getifaddrs failure: " + errnoWithDescription(errno), status == 0);

            vector<string> out;

            // based on example code from linux getifaddrs manpage
            for (ifaddrs * addr = addrs; addr != NULL; addr = addr->ifa_next){
                if ( addr->ifa_addr == NULL ) continue;
                int family = addr->ifa_addr->sa_family;
                char host[NI_MAXHOST];

                if (family == AF_INET || family == AF_INET6) {
                   status = getnameinfo(addr->ifa_addr,
                                        (family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)),
                                        host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
                   if ( status != 0 ){
                       freeifaddrs( addrs );
                       addrs = NULL;
                       msgasserted( 13470, string("getnameinfo() failed: ") + gai_strerror(status) );
                   }

                   out.push_back(host);
               }

            }

            freeifaddrs( addrs );
            addrs = NULL;

            if (logLevel >= 1){ 
                log(1) << "getMyAddrs():";
                for (vector<string>::const_iterator it=out.begin(), end=out.end(); it!=end; ++it){
                    log(1) << " [" << *it << ']';
                }
                log(1) << endl;
            }

            return out;
        }

        vector<string> getAllIPs(StringData iporhost){
            addrinfo* addrs = NULL;
            addrinfo hints;
            memset(&hints, 0, sizeof(addrinfo));
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_family = (IPv6Enabled() ? AF_UNSPEC : AF_INET);

            static string portNum = BSONObjBuilder::numStr(cmdLine.port);

            int ret = getaddrinfo(iporhost.data(), portNum.c_str(), &hints, &addrs);
            massert(13471, string("getaddrinfo(\"") + iporhost.data() + "\") failed: " + gai_strerror(ret), ret == 0);

            vector<string> out;
            for (addrinfo* addr = addrs; addr != NULL; addr = addr->ai_next){
                int family = addr->ai_family;
                char host[NI_MAXHOST];

                if (family == AF_INET || family == AF_INET6) {
                    int status = getnameinfo(addr->ai_addr, addr->ai_addrlen, host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);

                    massert(13472, string("getnameinfo() failed: ") + gai_strerror(status), status == 0);

                    out.push_back(host);
                }

            }

            freeaddrinfo(addrs);

            if (logLevel >= 1){ 
                log(1) << "getallIPs(\"" << iporhost << "\"):";
                for (vector<string>::const_iterator it=out.begin(), end=out.end(); it!=end; ++it){
                    log(1) << " [" << *it << ']';
                }
                log(1) << endl;
            }

            return out;
        }
#endif
    }
    
    bool HostAndPort::isSelf() const { 
        int p = _port == -1 ? CmdLine::DefaultDBPort : _port;

        if( p != cmdLine.port ){
            return false;
        } else {
            map<string, bool>::const_iterator it = isSelfCache.find(_host);
            if (it != isSelfCache.end()){
                return it->second;
            }

#if !defined(_WIN32) && !defined(__sunos__)
            
            static const vector<string> myaddrs = getMyAddrs();
            const vector<string> addrs = getAllIPs(_host);

            bool ret=false;
            for (vector<string>::const_iterator i=myaddrs.begin(), iend=myaddrs.end(); i!=iend; ++i){
                for (vector<string>::const_iterator j=addrs.begin(), jend=addrs.end(); j!=jend; ++j){
                    string a = *i;
                    string b = *j;

                    if ( a == b ||
                         ( a.find( "127." ) == 0 && b.find( "127." ) == 0 )  // 127. is all loopback
                         ){
                        ret = true;
                        break;
                    }
                }
            }

#else
            SockAddr addr (_host.c_str(), 0); // port 0 is dynamically assigned
            SOCKET sock = ::socket(addr.getType(), SOCK_STREAM, 0);
            assert(sock != INVALID_SOCKET);

            bool ret = (::bind(sock, addr.raw(), addr.addressSize) == 0);
            closesocket(sock);
#endif

            isSelfCache[_host] = ret;

            return ret;
        }
    }

} // namespace mongo
