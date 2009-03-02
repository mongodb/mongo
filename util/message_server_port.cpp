// message_server_port.cpp

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
            Listener( port ){
            
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
            listen();
        }

    };


    MessageServer * createServer( int port , MessageHandler * handler ){
        return new PortMessageServer( port , handler );
    }    

}

#endif
