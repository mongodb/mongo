// message_server_asio.cpp

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

#ifdef USE_ASIO

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/shared_ptr.hpp>

#include <iostream>
#include <vector>

#include "message.h"
#include "message_server.h"
#include "../util/concurrency/mvar.h"

using namespace boost;
using namespace boost::asio;
using namespace boost::asio::ip;

namespace mongo {
    class MessageServerSession;

    namespace {
        class StickyThread {
        public:
            StickyThread()
                : _thread(boost::ref(*this))
            {}

            ~StickyThread() {
                _mss.put(boost::shared_ptr<MessageServerSession>());
                _thread.join();
            }

            void ready(boost::shared_ptr<MessageServerSession> mss) {
                _mss.put(mss);
            }

            void operator() () {
                boost::shared_ptr<MessageServerSession> mss;
                while((mss = _mss.take())) { // intentionally not using ==
                    task(mss.get());
                    mss.reset();
                }
            }

        private:
            boost::thread _thread;
            inline void task(MessageServerSession* mss); // must be defined after MessageServerSession

            MVar<boost::shared_ptr<MessageServerSession> > _mss; // populated when given a task
        };

        vector<boost::shared_ptr<StickyThread> > thread_pool;
        mongo::mutex tp_mutex; // this is only needed if io_service::run() is called from multiple threads
    }

    class MessageServerSession : public boost::enable_shared_from_this<MessageServerSession> , public AbstractMessagingPort {
    public:
        MessageServerSession( MessageHandler * handler , io_service& ioservice )
            : _handler( handler )
            , _socket( ioservice )
            , _portCache(0)
        { }

        ~MessageServerSession() {
            cout << "disconnect from: " << _socket.remote_endpoint() << endl;
        }

        tcp::socket& socket() {
            return _socket;
        }

        void start() {
            cout << "MessageServerSession start from:" << _socket.remote_endpoint() << endl;
            _startHeaderRead();
        }

        void handleReadHeader( const boost::system::error_code& error ) {
            if ( _inHeader.len == 0 )
                return;

            if ( ! _inHeader.valid() ) {
                cout << "  got invalid header from: " << _socket.remote_endpoint() << " closing connected" << endl;
                return;
            }

            char * raw = (char*)malloc( _inHeader.len );

            MsgData * data = (MsgData*)raw;
            memcpy( data , &_inHeader , sizeof( _inHeader ) );
            verify( data->len == _inHeader.len );

            uassert( 10273 ,  "_cur not empty! pipelining requests not supported" , ! _cur.data );

            _cur.setData( data , true );
            async_read( _socket ,
                        buffer( raw + sizeof( _inHeader ) , _inHeader.len - sizeof( _inHeader ) ) ,
                        boost::bind( &MessageServerSession::handleReadBody , shared_from_this() , boost::asio::placeholders::error ) );
        }

        void handleReadBody( const boost::system::error_code& error ) {
            if (!_myThread) {
                mongo::mutex::scoped_lock(tp_mutex);
                if (!thread_pool.empty()) {
                    _myThread = thread_pool.back();
                    thread_pool.pop_back();
                }
            }

            if (!_myThread) // pool is empty
                _myThread.reset(new StickyThread());

            verify(_myThread);

            _myThread->ready(shared_from_this());
        }

        void process() {
            _handler->process( _cur , this );

            if (_reply.data) {
                async_write( _socket ,
                             buffer( (char*)_reply.data , _reply.data->len ) ,
                             boost::bind( &MessageServerSession::handleWriteDone , shared_from_this() , boost::asio::placeholders::error ) );
            }
            else {
                _cur.reset();
                _startHeaderRead();
            }
        }

        void handleWriteDone( const boost::system::error_code& error ) {
            {
                // return thread to pool after we have sent data to the client
                mongo::mutex::scoped_lock(tp_mutex);
                verify(_myThread);
                thread_pool.push_back(_myThread);
                _myThread.reset();
            }
            _cur.reset();
            _reply.reset();
            _startHeaderRead();
        }

        virtual void reply( Message& received, Message& response ) {
            reply( received , response , received.data->id );
        }

        virtual void reply( Message& query , Message& toSend, MSGID responseTo ) {
            _reply = toSend;

            _reply.data->id = nextMessageId();
            _reply.data->responseTo = responseTo;
            uassert( 10274 ,  "pipelining requests doesn't work yet" , query.data->id == _cur.data->id );
        }


        virtual unsigned remotePort() {
            if (!_portCache)
                _portCache = _socket.remote_endpoint().port(); //this is expensive
            return _portCache;
        }

    private:

        void _startHeaderRead() {
            _inHeader.len = 0;
            async_read( _socket ,
                        buffer( &_inHeader , sizeof( _inHeader ) ) ,
                        boost::bind( &MessageServerSession::handleReadHeader , shared_from_this() , boost::asio::placeholders::error ) );
        }

        MessageHandler * _handler;
        tcp::socket _socket;
        MsgData _inHeader;
        Message _cur;
        Message _reply;

        unsigned _portCache;

        boost::shared_ptr<StickyThread> _myThread;
    };

    void StickyThread::task(MessageServerSession* mss) {
        mss->process();
    }


    class AsyncMessageServer : public MessageServer {
    public:
        // TODO accept an IP address to bind to
        AsyncMessageServer( const MessageServer::Options& opts , MessageHandler * handler )
            : _port( opts.port )
            , _handler(handler)
            , _endpoint( tcp::v4() , opts.port )
            , _acceptor( _ioservice , _endpoint ) {
            _accept();
        }
        virtual ~AsyncMessageServer() {

        }

        void run() {
            cout << "AsyncMessageServer starting to listen on: " << _port << endl;
            boost::thread other(boost::bind(&io_service::run, &_ioservice));
            _ioservice.run();
            cout << "AsyncMessageServer done listening on: " << _port << endl;
        }

        void handleAccept( shared_ptr<MessageServerSession> session ,
                           const boost::system::error_code& error ) {
            if ( error ) {
                cout << "handleAccept error!" << endl;
                return;
            }
            session->start();
            _accept();
        }

        void _accept( ) {
            shared_ptr<MessageServerSession> session( new MessageServerSession( _handler , _ioservice ) );
            _acceptor.async_accept( session->socket() ,
                                    boost::bind( &AsyncMessageServer::handleAccept,
                                                 this,
                                                 session,
                                                 boost::asio::placeholders::error )
                                  );
        }

    private:
        int _port;
        MessageHandler * _handler;
        io_service _ioservice;
        tcp::endpoint _endpoint;
        tcp::acceptor _acceptor;
    };

    MessageServer * createServer( const MessageServer::Options& opts , MessageHandler * handler ) {
        return new AsyncMessageServer( opts , handler );
    }

}

#endif
