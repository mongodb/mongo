// message_server_port.cpp

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

#ifndef USE_ASIO

#include "message.h"
#include "message_port.h"
#include "message_server.h"
#include "listen.h"

#include "../../db/cmdline.h"
#include "../../db/lasterror.h"
#include "../../db/stats/counters.h"
#include "mongo/util/concurrency/remap_lock.h"
#include "mongo/util/concurrency/ticketholder.h"

#ifdef __linux__  // TODO: consider making this ifndef _WIN32
# include <sys/resource.h>
#endif

namespace mongo {

    namespace pms {

        MessageHandler * handler;

        void threadRun( MessagingPort * inPort) {
            TicketHolderReleaser connTicketReleaser( &connTicketHolder );

            setThreadName( "conn" );
            
            verify( inPort );
            inPort->psock->setLogLevel(1);
            scoped_ptr<MessagingPort> p( inPort );

            p->psock->postFork();

            string otherSide;

            Message m;
            try {
                LastError * le = new LastError();
                lastError.reset( le ); // lastError now has ownership

                otherSide = p->psock->remoteString();

                handler->connected( p.get() );

                while ( ! inShutdown() ) {
                    m.reset();
                    p->psock->clearCounters();

                    if ( ! p->recv(m) ) {
                        if( !cmdLine.quiet ){
                            int conns = connTicketHolder.used()-1;
                            const char* word = (conns == 1 ? " connection" : " connections");
                            log() << "end connection " << otherSide << " (" << conns << word << " now open)" << endl;
                        }
                        p->shutdown();
                        break;
                    }

                    handler->process( m , p.get() , le );
                    networkCounter.hit( p->psock->getBytesIn() , p->psock->getBytesOut() );
                }
            }
            catch ( AssertionException& e ) {
                log() << "AssertionException handling request, closing client connection: " << e << endl;
                p->shutdown();
            }
            catch ( SocketException& e ) {
                log() << "SocketException handling request, closing client connection: " << e << endl;
                p->shutdown();
            }
            catch ( const DBException& e ) { // must be right above std::exception to avoid catching subclasses
                log() << "DBException handling request, closing client connection: " << e << endl;
                p->shutdown();
            }
            catch ( std::exception &e ) {
                error() << "Uncaught std::exception: " << e.what() << ", terminating" << endl;
                dbexit( EXIT_UNCAUGHT );
            }
            catch ( ... ) {
                error() << "Uncaught exception, terminating" << endl;
                dbexit( EXIT_UNCAUGHT );
            }

            handler->disconnected( p.get() );
        }

    }

    class PortMessageServer : public MessageServer , public Listener {
    public:
        PortMessageServer(  const MessageServer::Options& opts, MessageHandler * handler ) :
            Listener( "" , opts.ipList, opts.port ) {

            uassert( 10275 ,  "multiple PortMessageServer not supported" , ! pms::handler );
            pms::handler = handler;
        }

        virtual void acceptedMP(MessagingPort * p) {

            if ( ! connTicketHolder.tryAcquire() ) {
                log() << "connection refused because too many open connections: " << connTicketHolder.used() << endl;

                // TODO: would be nice if we notified them...
                p->shutdown();
                delete p;

                sleepmillis(2); // otherwise we'll hard loop
                return;
            }

            try {
#ifndef __linux__  // TODO: consider making this ifdef _WIN32
                {
#ifdef _WIN32
                    // This Windows-only lock is to protect MemoryMappedFile::remapPrivateView ...
                    //  it unmaps and remaps the private map and needs to get the previous address,
                    //  and if we let a new thread get created between those calls, its thread
                    //  stack could be created within that block, leading to an fassert ...
                    RemapLock lk;
#endif
                    boost::thread thr( boost::bind( &pms::threadRun , p ) );
                }
#else
                pthread_attr_t attrs;
                pthread_attr_init(&attrs);
                pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);

                static const size_t STACK_SIZE = 1024*1024; // if we change this we need to update the warning

                struct rlimit limits;
                verify(getrlimit(RLIMIT_STACK, &limits) == 0);
                if (limits.rlim_cur > STACK_SIZE) {
                    pthread_attr_setstacksize(&attrs, (DEBUG_BUILD
                                                        ? (STACK_SIZE / 2)
                                                        : STACK_SIZE));
                } else if (limits.rlim_cur < 1024*1024) {
                    warning() << "Stack size set to " << (limits.rlim_cur/1024) << "KB. We suggest 1MB" << endl;
                }


                pthread_t thread;
                int failed = pthread_create(&thread, &attrs, (void*(*)(void*)) &pms::threadRun, p);

                pthread_attr_destroy(&attrs);

                if (failed) {
                    log() << "pthread_create failed: " << errnoWithDescription(failed) << endl;
                    throw boost::thread_resource_error(); // for consistency with boost::thread
                }
#endif
            }
            catch ( boost::thread_resource_error& ) {
                connTicketHolder.release();
                log() << "can't create new thread, closing connection" << endl;

                p->shutdown();
                delete p;

                sleepmillis(2);
            }
            catch ( ... ) {
                connTicketHolder.release();
                log() << "unknown error accepting new socket" << endl;

                p->shutdown();
                delete p;

                sleepmillis(2);
            }

        }

        virtual void setAsTimeTracker() {
            Listener::setAsTimeTracker();
        }

        void run() {
            initAndListen();
        }

        virtual bool useUnixSockets() const { return true; }
    };


    MessageServer * createServer( const MessageServer::Options& opts , MessageHandler * handler ) {
        return new PortMessageServer( opts , handler );
    }

}

#endif
