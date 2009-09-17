// message_server_asio.cpp

#ifdef USE_ASIO

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/shared_ptr.hpp>

#include <iostream>
#include <vector>

#include "message.h"
#include "message_server.h"

using namespace boost;
using namespace boost::asio;
using namespace boost::asio::ip;
using namespace std;

namespace mongo {

    class MessageServerSession : public enable_shared_from_this<MessageServerSession> , public AbstractMessagingPort {
    public:
        MessageServerSession( MessageHandler * handler , io_service& ioservice ) : _handler( handler ) , _socket( ioservice ){
            
        }
        ~MessageServerSession(){
            cout << "disconnect from: " << _socket.remote_endpoint() << endl;
        }

        tcp::socket& socket(){
            return _socket;
        }

        void start(){
            cout << "MessageServerSession start from:" << _socket.remote_endpoint() << endl;
            _startHeaderRead();
        }
        
        void handleReadHeader( const boost::system::error_code& error ){
            if ( _inHeader.len == 0 )
                return;

            if ( ! _inHeader.valid() ){
                cerr << "  got invalid header from: " << _socket.remote_endpoint() << " closing connected" << endl;
                return;
            }
            
            char * raw = (char*)malloc( _inHeader.len );
            
            MsgData * data = (MsgData*)raw;
            memcpy( data , &_inHeader , sizeof( _inHeader ) );
            assert( data->len == _inHeader.len );
            
            uassert( "_cur not empty! pipelining requests not supported" , ! _cur.data );

            _cur.setData( data , true );
            async_read( _socket , 
                        buffer( raw + sizeof( _inHeader ) , _inHeader.len - sizeof( _inHeader ) ) ,
                        bind( &MessageServerSession::handleReadBody , shared_from_this() , placeholders::error ) );
        }
        
        void handleReadBody( const boost::system::error_code& error ){
            _replyCalled = false;
            
            _handler->process( _cur , this );

            if ( ! _replyCalled ){
                _cur.reset();
                _startHeaderRead();
            }
        }
        
        void handleWriteDone( const boost::system::error_code& error ){
            _cur.reset();
            _replyCalled = false;
            _startHeaderRead();
        }
        
        virtual void reply( Message& received, Message& response ){
            reply( received , response , received.data->id );
        }
        
        virtual void reply( Message& query , Message& toSend, MSGID responseTo ){
            _replyCalled = true;

            toSend.data->id = nextMessageId();
            toSend.data->responseTo = responseTo;
            uassert( "pipelining requests doesn't work yet" , query.data->id == _cur.data->id );
            async_write( _socket , 
                         buffer( (char*)toSend.data , toSend.data->len ) , 
                         bind( &MessageServerSession::handleWriteDone , shared_from_this() , placeholders::error ) );
        }

        
        virtual unsigned remotePort(){
            return _socket.remote_endpoint().port();
        }
        
    private:        
        
        void _startHeaderRead(){
            _inHeader.len = 0;
            async_read( _socket , 
                        buffer( &_inHeader , sizeof( _inHeader ) ) ,
                        bind( &MessageServerSession::handleReadHeader , shared_from_this() , placeholders::error ) );
        }
        
        MessageHandler * _handler;
        tcp::socket _socket;
        MsgData _inHeader;
        Message _cur;

        bool _replyCalled;
    };
    

    class AsyncMessageServer : public MessageServer {
    public:
        AsyncMessageServer( int port , MessageHandler * handler ) : 
            MessageServer( port , handler ) , 
            _endpoint( tcp::v4() , port ) , 
            _acceptor( _ioservice , _endpoint ){
            _accept();
        }
        virtual ~AsyncMessageServer(){
            
        }

        void run(){
            cout << "AsyncMessageServer starting to listen on: " << _port << endl;
            _ioservice.run();
            cout << "AsyncMessageServer done listening on: " << _port << endl;
        }
        
        void handleAccept( shared_ptr<MessageServerSession> session , 
                           const boost::system::error_code& error ){
            if ( error ){
                cerr << "handleAccept error!" << endl;
                return;
            }
            session->start();
            _accept();
        }
        
        void _accept(){
            shared_ptr<MessageServerSession> session( new MessageServerSession( _handler , _ioservice ) );
            _acceptor.async_accept( session->socket() ,
                                    bind( &AsyncMessageServer::handleAccept,
                                          this, 
                                          session,
                                          boost::asio::placeholders::error )
                                    );            
        }

    private:
        io_service _ioservice;
        tcp::endpoint _endpoint;
        tcp::acceptor _acceptor;
    };

    MessageServer * createServer( int port , MessageHandler * handler ){
        return new AsyncMessageServer( port , handler );
    }    

}

#endif
