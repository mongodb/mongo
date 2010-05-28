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
#include "message_server.h"

#include "../db/cmdline.h"

namespace mongo {

    namespace pms {

        MessagingPort * grab = 0;
        MessageHandler * handler;
        
        void threadRun(){
            TicketHolderReleaser connTicketReleaser( &connTicketHolder );
        
            assert( grab );
            auto_ptr<MessagingPort> p( grab );
            grab = 0;
        
            string otherSide;
    
            Message m;
            try {
                otherSide = p->farEnd.toString();

                while ( 1 ){
                    m.reset();

                    if ( ! p->recv(m) ) {
                        if( !cmdLine.quiet )
                            log() << "end connection " << otherSide << endl;
                        p->shutdown();
                        break;
                    }
                    
                    handler->process( m , p.get() );
                }
            }
            catch ( const SocketException& ){
                log() << "unclean socket shutdown from: " << otherSide << endl;
            }
            catch ( const std::exception& e ){
                problem() << "uncaught exception (" << e.what() << ")(" << demangleName( typeid(e) ) <<") in PortMessageServer::threadRun, closing connection" << endl;
            }
            catch ( ... ){
                problem() << "uncaught exception in PortMessageServer::threadRun, closing connection" << endl;
            }            
            
            handler->disconnected( p.get() );
        }

    }

    class PortMessageServer : public MessageServer , public Listener {
    public:
            PortMessageServer(  const MessageServer::Options& opts, MessageHandler * handler ) :
            Listener( opts.ipList, opts.port ){
            
            uassert( 10275 ,  "multiple PortMessageServer not supported" , ! pms::handler );
            pms::handler = handler;
        }
        
        virtual void accepted(MessagingPort * p) {
            assert( ! pms::grab );
            pms::grab = p;
            
            if ( ! connTicketHolder.tryAcquire() ){
                log() << "connection refused because too many open connections" << endl;

                // TODO: would be nice if we notified them...
                p->shutdown();
                
                pms::grab = 0;
                sleepmillis(2); // otherwise we'll hard loop
                return;
            }

            try {
                boost::thread thr( pms::threadRun );
                while ( pms::grab ){
                    sleepmillis(1);
                }
            }
            catch ( boost::thread_resource_error& ){
                log() << "can't create new thread, closing connection" << endl;
                p->shutdown();
                pms::grab = 0;
                sleepmillis(2);
            }
        }
        
        void run(){
            initAndListen();
        }

    };


    MessageServer * createServer( const MessageServer::Options& opts , MessageHandler * handler ){
        return new PortMessageServer( opts , handler );
    }    

    TicketHolder connTicketHolder(20000);
}

#endif
