/**
*    Copyright (C) 2008 10gen Inc.
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
*/

/* message

   todo: authenticate; encrypt?
*/

#include "stdafx.h"
#include "message.h"
#include <time.h>
#include "../util/goodies.h"
#include <fcntl.h>
#include <errno.h>

namespace mongo {

    bool objcheck = false;
    
// if you want trace output:
#define mmm(x)

#ifdef MSG_NOSIGNAL
        const int portSendFlags = MSG_NOSIGNAL;
#else
        const int portSendFlags = 0;
#endif

    /* listener ------------------------------------------------------------------- */

    bool Listener::init() {
        SockAddr me;
        if ( ip.empty() )
            me = SockAddr( port );
        else
            me = SockAddr( ip.c_str(), port );
        sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if ( sock == INVALID_SOCKET ) {
            log() << "ERROR: listen(): invalid socket? " << errno << endl;
            return false;
        }
        prebindOptions( sock );
        if ( ::bind(sock, (sockaddr *) &me.sa, me.addressSize) != 0 ) {
            log() << "listen(): bind() failed errno:" << errno << endl;
            if ( errno == 98 )
                log() << "98 == addr already in use" << endl;
            closesocket(sock);
            return false;
        }

        if ( ::listen(sock, 128) != 0 ) {
            log() << "listen(): listen() failed " << errno << endl;
            closesocket(sock);
            return false;
        }
        
        return true;
    }

    void Listener::listen() {
        static long connNumber = 0;
        SockAddr from;
        while ( 1 ) {
            int s = accept(sock, (sockaddr *) &from.sa, &from.addressSize);
            if ( s < 0 ) {
                if ( errno == ECONNABORTED || errno == EBADF ) {
                    log() << "Listener on port " << port << " aborted" << endl;
                    return;
                }
                log() << "Listener: accept() returns " << s << " errno:" << errno << ", strerror: " << strerror( errno ) << endl;
                continue;
            }
            disableNagle(s);
            log() << "connection accepted from " << from.toString() << " #" << ++connNumber << endl;
            accepted( new MessagingPort(s, from) );
        }
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
            flush();
            delete( _cur );
        }

        void append( Message& m ) {
            assert( m.data->len <= 1300 );

            if ( len() + m.data->len > 1300 )
                flush();

            memcpy( _cur , m.data , m.data->len );
            _cur += m.data->len;
        }

        int flush() {
            if ( _buf == _cur )
                return 0;

            int x = ::send( _port->sock , _buf , len() , 0 );
            _cur = _buf;
            return x;
        }

        int len() {
            return _cur - _buf;
        }

    private:

        MessagingPort* _port;

        char * _buf;
        char * _cur;
    };

    class Ports { 
        set<MessagingPort*>& ports;
        boost::mutex& m;
    public:
        // we "new" this so it is still be around when other automatic global vars
        // are being destructed during termination.
        Ports() : ports( *(new set<MessagingPort*>()) ), 
            m( *(new boost::mutex()) ) { }
        void closeAll() { \
            boostlock bl(m);
            for ( set<MessagingPort*>::iterator i = ports.begin(); i != ports.end(); i++ )
                (*i)->shutdown();
        }
        void insert(MessagingPort* p) { 
            boostlock bl(m);
            ports.insert(p);
        }
        void erase(MessagingPort* p) { 
            boostlock bl(m);
            ports.erase(p);
        }
    } ports;



    void closeAllSockets() {
        ports.closeAll();
    }

    MessagingPort::MessagingPort(int _sock, SockAddr& _far) : sock(_sock), piggyBackData(0), farEnd(_far) {
        ports.insert(this);
    }

    MessagingPort::MessagingPort() {
        ports.insert(this);
        sock = -1;
        piggyBackData = 0;
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

} // namespace mongo

#include "../util/background.h"

namespace mongo {

    class ConnectBG : public BackgroundJob {
    public:
        int sock;
        int res;
        SockAddr farEnd;
        void run() {
            res = ::connect(sock, (sockaddr *) &farEnd.sa, farEnd.addressSize);
        }
    };

    bool MessagingPort::connect(SockAddr& _far)
    {
        farEnd = _far;

        sock = socket(AF_INET, SOCK_STREAM, 0);
        if ( sock == INVALID_SOCKET ) {
            log() << "ERROR: connect(): invalid socket? " << errno << endl;
            return false;
        }

#if 0
        long fl = fcntl(sock, F_GETFL, 0);
        assert( fl >= 0 );
        fl |= O_NONBLOCK;
        fcntl(sock, F_SETFL, fl);

        int res = ::connect(sock, (sockaddr *) &farEnd.sa, farEnd.addressSize);
        if ( res ) {
            if ( errno == EINPROGRESS )
                //log() << "connect(): failed errno:" << errno << ' ' << farEnd.getPort() << endl;
                closesocket(sock);
            sock = -1;
            return false;
        }

#endif

        ConnectBG bg;
        bg.sock = sock;
        bg.farEnd = farEnd;
        bg.go();

        // int res = ::connect(sock, (sockaddr *) &farEnd.sa, farEnd.addressSize);
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

        disableNagle(sock);

#ifdef SO_NOSIGPIPE
        // osx
        const int one = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(int));
#endif

        return true;
    }

    bool MessagingPort::recv(Message& m) {
again:
        mmm( out() << "*  recv() sock:" << this->sock << endl; )
        int len = -1;

        char *lenbuf = (char *) &len;
        int lft = 4;
        while ( 1 ) {
            int x = ::recv(sock, lenbuf, lft, 0);
            if ( x == 0 ) {
                DEV out() << "MessagingPort recv() conn closed? " << farEnd.toString() << endl;
                m.reset();
                return false;
            }
            if ( x < 0 ) {
                log() << "MessagingPort recv() error \"" << strerror( errno ) << "\" (" << errno << ") " << farEnd.toString()<<endl;
                m.reset();
                return false;
            }
            lft -= x;
            if ( lft == 0 )
                break;
            lenbuf += x;
            log() << "MessagingPort recv() got " << x << " bytes wanted 4, lft=" << lft << endl;
            assert( lft > 0 );
        }

        if ( len < 0 || len > 16000000 ) {
            if ( len == -1 ) {
                // Endian check from the database, after connecting, to see what mode server is running in.
                unsigned foo = 0x10203040;
                int x = ::send(sock, (char *) &foo, 4,  portSendFlags );
                if ( x <= 0 ) {
                    log() << "MessagingPort endian send() error " << errno << ' ' << farEnd.toString() << endl;
                    return false;
                }
                goto again;
            }
            log() << "bad recv() len: " << len << '\n';
            return false;
        }

        int z = (len+1023)&0xfffffc00;
        assert(z>=len);
        MsgData *md = (MsgData *) malloc(z);
        md->len = len;

        if ( len <= 0 ) {
            out() << "got a length of " << len << ", something is wrong" << endl;
            return false;
        }

        char *p = (char *) &md->id;
        int left = len -4;
        while ( 1 ) {
            int x = ::recv(sock, p, left, 0);
            if ( x == 0 ) {
                DEV out() << "MessagingPort::recv(): conn closed? " << farEnd.toString() << endl;
                m.reset();
                return false;
            }
            if ( x < 0 ) {
                log() << "MessagingPort recv() error " << errno << ' ' << farEnd.toString() << endl;
                m.reset();
                return false;
            }
            left -= x;
            p += x;
            if ( left <= 0 )
                break;
        }

        m.setData(md, true);
        return true;
    }

    void MessagingPort::reply(Message& received, Message& response) {
        say(/*received.from, */response, received.data->id);
    }

    void MessagingPort::reply(Message& received, Message& response, MSGID responseTo) {
        say(/*received.from, */response, responseTo);
    }

    bool MessagingPort::call(Message& toSend, Message& response) {
        mmm( out() << "*call()" << endl; )
        MSGID old = toSend.data->id;
        say(/*to,*/ toSend);
        while ( 1 ) {
            bool ok = recv(response);
            if ( !ok )
                return false;
            //out() << "got response: " << response.data->responseTo << endl;
            if ( response.data->responseTo == toSend.data->id )
                break;
            out() << "********************" << endl;
            out() << "ERROR: MessagingPort::call() wrong id got:" << (unsigned)response.data->responseTo << " expect:" << (unsigned)toSend.data->id << endl;
            out() << "  old:" << (unsigned)old << endl;
            out() << "  response msgid:" << (unsigned)response.data->id << endl;
            out() << "  response len:  " << (unsigned)response.data->len << endl;
            assert(false);
            response.reset();
        }
        mmm( out() << "*call() end" << endl; )
        return true;
    }

    void MessagingPort::say(Message& toSend, int responseTo) {
        mmm( out() << "*  say() sock:" << this->sock << " thr:" << GetCurrentThreadId() << endl; )
        toSend.data->id = nextMessageId();
        toSend.data->responseTo = responseTo;

        int x = -100;

        if ( piggyBackData && piggyBackData->len() ) {
            mmm( out() << "*     have piggy back" << endl; )
            if ( ( piggyBackData->len() + toSend.data->len ) > 1300 ) {
                // won't fit in a packet - so just send it off
                piggyBackData->flush();
            }
            else {
                piggyBackData->append( toSend );
                x = piggyBackData->flush();
            }
        }

        if ( x == -100 )
            x = ::send(sock, (char*)toSend.data, toSend.data->len , portSendFlags );
        
        if ( x <= 0 ) {
            log() << "MessagingPort say send() error " << errno << ' ' << farEnd.toString() << endl;
            throw SocketException();
        }

    }

    void MessagingPort::piggyBack( Message& toSend , int responseTo ) {

        if ( toSend.data->len > 1300 ) {
            // not worth saving because its almost an entire packet
            say( toSend );
            return;
        }

        // we're going to be storing this, so need to set it up
        toSend.data->id = nextMessageId();
        toSend.data->responseTo = responseTo;

        if ( ! piggyBackData )
            piggyBackData = new PiggyBackData( this );

        piggyBackData->append( toSend );
    }

    unsigned MessagingPort::remotePort(){
        return farEnd.getPort();
    }

    MSGID NextMsgId;
    bool usingClientIds = 0;
    ThreadLocalInt clientId;

    struct MsgStart {
        MsgStart() {
            NextMsgId = (((unsigned) time(0)) << 16) ^ curTimeMillis();
            assert(MsgDataHeaderSize == 16);
        }
    } msgstart;
    
    MSGID nextMessageId(){
        MSGID msgid = NextMsgId;
        ++NextMsgId;
        
        if ( usingClientIds ){
            msgid = msgid & 0xFFFF;
            msgid = msgid | clientId.get();
        }

        return msgid;
    }

    bool doesOpGetAResponse( int op ){
        return op == dbQuery || op == dbGetMore;
    }
    
    void setClientId( int id ){
        usingClientIds = true;
        id = id & 0xFFFF0000;
        massert( "invalid id" , id );
        clientId.reset( id );
    }
    
    int getClientId(){
        return clientId.get();
    }
    
} // namespace mongo
