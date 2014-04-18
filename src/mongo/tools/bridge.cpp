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

#include "mongo/pch.h"

#include <boost/thread.hpp>

#include "mongo/base/initializer.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbmessage.h"
#include "mongo/tools/mongobridge_options.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/net/message.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/text.h"
#include "mongo/util/timer.h"

using namespace mongo;
using namespace std;

void cleanup( int sig );

class Forwarder {
public:
    Forwarder( MessagingPort &mp ) : mp_( mp ) {
    }

    void operator()() const {
        DBClientConnection dest;
        string errmsg;

        Timer connectTimer;
        while (!dest.connect(mongoBridgeGlobalParams.destUri, errmsg)) {
            // If we can't connect for the configured timeout, give up
            //
            if (connectTimer.seconds() >= mongoBridgeGlobalParams.connectTimeoutSec) {
                cout << "Unable to establish connection from " << mp_.psock->remoteString() 
                     << " to " << mongoBridgeGlobalParams.destUri 
                     << " after " << connectTimer.seconds() << " seconds. Giving up." << endl;
                mp_.shutdown();
                return;
            }

            sleepmillis(500);
        }

        Message m;
        while( 1 ) {
            try {
                m.reset();
                if ( !mp_.recv( m ) ) {
                    cout << "end connection " << mp_.psock->remoteString() << endl;
                    mp_.shutdown();
                    break;
                }
                sleepmillis(mongoBridgeGlobalParams.delay);

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

set<MessagingPort*>& ports ( *(new std::set<MessagingPort*>()) );

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
    ::_exit( 0 );
}
#if !defined(_WIN32)
void myterminate() {
    printStackTrace(severe().stream() << "bridge terminate() called, printing stack:\n");
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

int toolMain( int argc, char **argv, char** envp ) {
    mongo::runGlobalInitializersOrDie(argc, argv, envp);

    static StaticObserver staticObserver;

    setupSignals();

    listener.reset(new MyListener(mongoBridgeGlobalParams.port));
    listener->setupSockets();
    listener->initAndListen();

    return 0;
}

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables toolMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    WindowsCommandLine wcl(argc, argvW, envpW);
    int exitCode = toolMain(argc, wcl.argv(), wcl.envp());
    ::_exit(exitCode);
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = toolMain(argc, argv, envp);
    ::_exit(exitCode);
}
#endif
