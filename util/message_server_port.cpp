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

#ifndef USE_ASIO

#include "message.h"
#include "message_server.h"

namespace mongo {

    namespace pms {

        MessagingPort * grab = 0;
        MessageHandler * handler;

        void threadRun(){
            assert( grab );
            MessagingPort * p = grab;
            grab = 0;
            
            Message m;
            try {
                while ( 1 ){
                    m.reset();

                    if ( ! p->recv(m) ) {
                        log() << "end connection " << p->farEnd.toString() << endl;
                        p->shutdown();
                        break;
                    }
                    
                    handler->process( m , p );
                }
            }
            catch ( ... ){
                problem() << "uncaught exception in PortMessageServer::threadRun, closing connection" << endl;
                delete p;
            }            
            
        }

    }

    class PortMessageServer : public MessageServer , public Listener {
    public:
        PortMessageServer( int port , MessageHandler * handler ) :
            MessageServer( port , handler ) , 
            Listener( "", port ){
            
            uassert( "multiple PortMessageServer not supported" , ! pms::handler );
            pms::handler = handler;
        }
        
        virtual void accepted(MessagingPort * p) {
            assert( ! pms::grab );
            pms::grab = p;
            boost::thread thr( pms::threadRun );
            while ( pms::grab )
                sleepmillis(1);
        }
        
        void run(){
            assert( init() );
            listen();
        }

    };


    MessageServer * createServer( int port , MessageHandler * handler ){
        return new PortMessageServer( port , handler );
    }    

}

#endif
