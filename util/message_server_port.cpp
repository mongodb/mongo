// message_server_port.cpp

#ifndef USE_ASIO

#include "message.h"
#include "message_server.h"

namespace mongo {

    class PortMessageServer : public MessageServer , public Listener {
    public:
        PortMessageServer( int port , MessageHandler * handler ) :
            MessageServer( port , handler ) , 
            Listener( port ){

        }
        
        void threadRun( MessagingPort * p ){
            assert( p );
            Message m;
            try {
                while ( 1 ){
                    m.reset();

                    if ( ! p->recv(m) ) {
                        log() << "end connection " << p->farEnd.toString() << endl;
                        p->shutdown();
                        break;
                    }
                    
                    _handler->process( m , p );
                }
            }
            catch ( ... ){
                problem() << "uncaught exception in PortMessageServer::threadRun, closing connection" << endl;
                delete p;
            }
        }
        
        virtual void accepted(MessagingPort * p) {
            boost::thread thr( bind( &PortMessageServer::threadRun , this , p ) );
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
