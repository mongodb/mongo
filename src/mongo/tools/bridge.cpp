// bridge.cpp

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

#include "pch.h"
#include "mongo/db/dbmessage.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/net/message.h"
#include "mongo/util/util.h"

using namespace mongo;
using namespace std;

int port = 0;
string destUri;
void cleanup( int sig );

class Forwarder {
public:
    Forwarder( MessagingPort &mp ) : mp_( mp ) {
    }
    void operator()() const {
        DBClientConnection dest;
        string errmsg;
        while( !dest.connect( destUri, errmsg ) )
            sleepmillis( 500 );
        Message m;
        while( 1 ) {
            try {
                m.reset();
                if ( !mp_.recv( m ) ) {
                    cout << "end connection " << mp_.psock->remoteString() << endl;
                    mp_.shutdown();
                    break;
                }

                int oldId = m.header()->id;
                if ( m.operation() == dbQuery || m.operation() == dbMsg || m.operation() == dbGetMore ) {
                    bool exhaust = false;
                    if ( m.operation() == dbQuery ) {
                        DbMessage d( m );
                        QueryMessage q( d );
                        exhaust = q.queryOptions & QueryOption_Exhaust;
                    }
                    Message response;
                    dest.port().call( m, response );

                    // nothing to reply with?
                    if ( response.empty() ) cleanup(0);

                    mp_.reply( m, response, oldId );
                    while ( exhaust ) {
                        MsgData *header = response.header();
                        QueryResult *qr = (QueryResult *) header;
                        if ( qr->cursorId ) {
                            response.reset();
                            dest.port().recv( response );
                            mp_.reply( m, response ); // m argument is ignored anyway
                        }
                        else {
                            exhaust = false;
                        }
                    }
                }
                else {
                    dest.port().say( m, oldId );
                }
            }
            catch ( ... ) {
                log() << "caught exception in Forwarder, continuing" << endl;
            }
        }
    }
private:
    MessagingPort &mp_;
};

set<MessagingPort*> ports;

class MyListener : public Listener {
public:
    MyListener( int port ) : Listener( "bridge" , "", port ) {}
    virtual void acceptedMP(MessagingPort *mp) {
        ports.insert( mp );
        Forwarder f( *mp );
        boost::thread t( f );
    }
};

auto_ptr< MyListener > listener;


void cleanup( int sig ) {
    ListeningSockets::get()->closeAll();
    for ( set<MessagingPort*>::iterator i = ports.begin(); i != ports.end(); i++ )
        (*i)->shutdown();
    ::exit( 0 );
}
#if !defined(_WIN32)
void myterminate() {
    rawOut( "bridge terminate() called, printing stack:" );
    printStackTrace();
    ::abort();
}

void setupSignals() {
    signal( SIGINT , cleanup );
    signal( SIGTERM , cleanup );
    signal( SIGPIPE , cleanup );
    signal( SIGABRT , cleanup );
    signal( SIGSEGV , cleanup );
    signal( SIGBUS , cleanup );
    signal( SIGFPE , cleanup );
    set_terminate( myterminate );
}
#else
inline void setupSignals() {}
#endif

void helpExit() {
    cout << "usage mongobridge --port <port> --dest <destUri>" << endl;
    cout << "    port: port to listen for mongo messages" << endl;
    cout << "    destUri: uri of remote mongod instance" << endl;
    ::exit( -1 );
}

void check( bool b ) {
    if ( !b )
        helpExit();
}

int main( int argc, char **argv ) {
    static StaticObserver staticObserver;

    setupSignals();

    check( argc == 5 );

    for( int i = 1; i < 5; ++i ) {
        check( i % 2 != 0 );
        if ( strcmp( argv[ i ], "--port" ) == 0 ) {
            port = strtol( argv[ ++i ], 0, 10 );
        }
        else if ( strcmp( argv[ i ], "--dest" ) == 0 ) {
            destUri = argv[ ++i ];
        }
        else {
            check( false );
        }
    }
    check( port != 0 && !destUri.empty() );

    listener.reset( new MyListener( port ) );
    listener->initAndListen();

    return 0;
}
