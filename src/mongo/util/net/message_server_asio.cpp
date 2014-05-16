// message_server_asio.cpp

/*    Copyright 2009 10gen Inc.
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

#ifdef USE_ASIO

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/shared_ptr.hpp>

#include <iostream>
#include <vector>

#include "mongo/util/concurrency/mvar.h"
#include "mongo/util/message.h"
#include "mongo/util/message_server.h"

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
