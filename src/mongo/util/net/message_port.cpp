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
#include <time.h>

#include "message.h"
#include "message_port.h"
#include "listen.h"

#include "../goodies.h"
#include "../background.h"
#include "../time_support.h"
#include "../../db/cmdline.h"
#include "../scopeguard.h"


#ifndef _WIN32
# ifndef __sunos__
#  include <ifaddrs.h>
# endif
# include <sys/resource.h>
# include <sys/stat.h>
#endif

namespace mongo {


// if you want trace output:
#define mmm(x)

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
            verify( m.header()->len <= 1300 );

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

    MessagingPort::MessagingPort(int fd, const SockAddr& remote) 
        : psock( new Socket( fd , remote ) ) , piggyBackData(0) {
        ports.insert(this);
    }

    MessagingPort::MessagingPort( double timeout, int ll ) 
        : psock( new Socket( timeout, ll ) ) {
        ports.insert(this);
        piggyBackData = 0;
    }

    MessagingPort::MessagingPort( boost::shared_ptr<Socket> sock )
        : psock( sock ), piggyBackData( 0 ) {
        ports.insert(this);
    }

    void MessagingPort::shutdown() {
        psock->close();
    }

    MessagingPort::~MessagingPort() {
        if ( piggyBackData )
            delete( piggyBackData );
        shutdown();
        ports.erase(this);
    }

    bool MessagingPort::recv(Message& m) {
        try {
again:
            //mmm( log() << "*  recv() sock:" << this->sock << endl; )
            const int lft = 4;
            char lenbuf[lft];
            psock->recv( lenbuf, lft );
            int len = little<int>::ref( lenbuf );

            if ( len < 16 || len > 48000000 ) { // messages must be large enough for headers
                if ( len == -1 ) {
                    // Endian check from the client, after connecting, to see what mode server is running in.
                    unsigned foo = 0x10203040;
                    send( (char *) &foo, 4, "endian" );
                    goto again;
                }

                if ( len == 542393671 ) {
                    // an http GET
                    log( psock->getLogLevel() ) << "looks like you're trying to access db over http on native driver port.  please add 1000 for webserver" << endl;
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
            verify(z>=len);
            MsgData *md = (MsgData *) malloc(z);
            ScopeGuard guard = MakeGuard(free, md);
            verify(md);
            md->len = len;

            char *p = (char *) &md->id;
            int left = len -4;

            psock->recv( p, left );

            guard.Dismiss();
            m.setData(md, true);
            return true;

        }
        catch ( const SocketException & e ) {
            log(psock->getLogLevel() + (e.shouldPrint() ? 0 : 1) ) << "SocketException: remote: " << remote() << " error: " << e << endl;
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
            if ( !ok ) {
                mmm( log() << "recv not ok" << endl; )
                return false;
            }
            //log() << "got response: " << response.data->responseTo << endl;
            if ( response.header()->responseTo == toSend.header()->id )
                break;
            error() << "MessagingPort::call() wrong id got:" << hex << (unsigned)response.header()->responseTo << " expect:" << (unsigned)toSend.header()->id << '\n'
                    << dec
                    << "  toSend op: " << (unsigned)toSend.operation() << '\n'
                    << "  response msgid:" << (unsigned)response.header()->id << '\n'
                    << "  response len:  " << (unsigned)response.header()->len << '\n'
                    << "  response op:  " << response.operation() << '\n'
                    << "  remote: " << psock->remoteString() << endl;
            verify(false);
            response.reset();
        }
        mmm( log() << "*call() end" << endl; )
        return true;
    }

    void MessagingPort::assertStillConnected() { 
        uassert(15901, "client disconnected during operation", psock->stillConnected());
    }

    void MessagingPort::say(Message& toSend, int responseTo) {
        verify( !toSend.empty() );
        mmm( log() << "*  say()  thr:" << GetCurrentThreadId() << endl; )
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

    HostAndPort MessagingPort::remote() const {
        if ( ! _remoteParsed.hasPort() )
            _remoteParsed = HostAndPort( psock->remoteAddr() );
        return _remoteParsed;
    }


} // namespace mongo
