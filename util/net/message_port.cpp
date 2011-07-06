// message_port.cpp

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

#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "message.h"
#include "message_port.h"
#include "listen.h"

#include "../goodies.h"
#include "../background.h"
#include "../time_support.h"
#include "../../db/cmdline.h"
#include "../../client/dbclient.h"


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


// if you want trace output:
#define mmm(x)

#ifdef MSG_NOSIGNAL
    const int portSendFlags = MSG_NOSIGNAL;
    const int portRecvFlags = MSG_NOSIGNAL;
#else
    const int portSendFlags = 0;
    const int portRecvFlags = 0;
#endif


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

    MessagingPort::MessagingPort(int _sock, const SockAddr& _far) : sock(_sock), piggyBackData(0), _bytesIn(0), _bytesOut(0), farEnd(_far), _timeout() {
        _logLevel = 0;
        ports.insert(this);
    }

    MessagingPort::MessagingPort( double timeout, int ll ) : _bytesIn(0), _bytesOut(0) {
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
        ConnectBG(int sock, SockAddr farEnd) : _sock(sock), _farEnd(farEnd) { }

        void run() { _res = ::connect(_sock, _farEnd.raw(), _farEnd.addressSize); }
        string name() const { return "ConnectBG"; }
        int inError() const { return _res; }

    private:
        int _sock;
        int _res;
        SockAddr _farEnd;
    };

    bool MessagingPort::connect(SockAddr& _far) {
        farEnd = _far;

        sock = socket(farEnd.getType(), SOCK_STREAM, 0);
        if ( sock == INVALID_SOCKET ) {
            log(_logLevel) << "ERROR: connect invalid socket " << errnoWithDescription() << endl;
            return false;
        }

        if ( _timeout > 0 ) {
            setSockTimeouts( sock, _timeout );
        }

        ConnectBG bg(sock, farEnd);
        bg.go();
        if ( bg.wait(5000) ) {
            if ( bg.inError() ) {
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

                if ( len == 542393671 ) {
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
            }
            catch (...) {
                free(md);
                throw;
            }

            _bytesIn += len;
            m.setData(md, true);
            return true;

        }
        catch ( const SocketException & e ) {
            log(_logLevel + (e.shouldPrint() ? 0 : 1) ) << "SocketException: remote: " << remote() << " error: " << e << endl;
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
        say(toSend);
        return recv( toSend , response );
    }

    bool MessagingPort::recv( const Message& toSend , Message& response ) {
        while ( 1 ) {
            bool ok = recv(response);
            if ( !ok )
                return false;
            //log() << "got response: " << response.data->responseTo << endl;
            if ( response.header()->responseTo == toSend.header()->id )
                break;
            error() << "MessagingPort::call() wrong id got:" << hex << (unsigned)response.header()->responseTo << " expect:" << (unsigned)toSend.header()->id << '\n'
                    << dec
                    << "  toSend op: " << (unsigned)toSend.operation() << '\n'
                    << "  response msgid:" << (unsigned)response.header()->id << '\n'
                    << "  response len:  " << (unsigned)response.header()->len << '\n'
                    << "  response op:  " << response.operation() << '\n'
                    << "  farEnd: " << farEnd << endl;
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
        _bytesOut += len;
        while( len > 0 ) {
            int ret = ::send( sock , data , len , portSendFlags );
            if ( ret == -1 ) {
                if ( ( errno == EAGAIN || errno == EWOULDBLOCK ) && _timeout != 0 ) {
                    log(_logLevel) << "MessagingPort " << context << " send() timed out " << farEnd.toString() << endl;
                    throw SocketException( SocketException::SEND_TIMEOUT , remote() );
                }
                else {
                    SocketException::Type t = SocketException::SEND_ERROR;
#if defined(_WINDOWS)
                    if( e == WSAETIMEDOUT ) t = SocketException::SEND_TIMEOUT;
#endif
                    log(_logLevel) << "MessagingPort " << context << " send() " << errnoWithDescription() << ' ' << farEnd.toString() << endl;
                    throw SocketException( t , remote() );
                }
            }
            else {
                assert( ret <= len );
                len -= ret;
                data += ret;
            }
        }
    }

    // sends all data or throws an exception
    void MessagingPort::send( const vector< pair< char *, int > > &data, const char *context ) {
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
                    throw SocketException( SocketException::SEND_ERROR , remote() );
                }
                else {
                    log(_logLevel) << "MessagingPort " << context << " send() remote timeout " << farEnd.toString() << endl;
                    throw SocketException( SocketException::SEND_TIMEOUT , remote() );
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

    void MessagingPort::recv( char * buf , int len ) {
        unsigned retries = 0;
        while( len > 0 ) {
            int ret = ::recv( sock , buf , len , portRecvFlags );
            if ( ret > 0 ) {
                if ( len <= 4 && ret != len )
                    log(_logLevel) << "MessagingPort recv() got " << ret << " bytes wanted len=" << len << endl;
                assert( ret <= len );
                len -= ret;
                buf += ret;
            }
            else if ( ret == 0 ) {
                log(3) << "MessagingPort recv() conn closed? " << farEnd.toString() << endl;
                throw SocketException( SocketException::CLOSED , remote() );
            }
            else { /* ret < 0  */
                int e = errno;
                
#if defined(EINTR) && !defined(_WIN32)
                if( e == EINTR ) {
                    if( ++retries == 1 ) {
                        log() << "EINTR retry" << endl;
                        continue;
                    }
                }
#endif
                if ( ( e == EAGAIN 
#ifdef _WINDOWS
                       || e == WSAETIMEDOUT
#endif
                       ) && _timeout > 0 ) {
                    // this is a timeout
                    log(_logLevel) << "MessagingPort recv() timeout  " << farEnd.toString() <<endl;
                    throw SocketException( SocketException::RECV_TIMEOUT, remote() );                    
                }

                log(_logLevel) << "MessagingPort recv() " << errnoWithDescription(e) << " " << farEnd.toString() <<endl;
                throw SocketException( SocketException::RECV_ERROR , remote() );
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
        if ( ! _farEndParsed.hasPort() )
            _farEndParsed = HostAndPort( farEnd );
        return _farEndParsed;
    }


} // namespace mongo
