// message_port.h

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

#pragma once

#include "sock.h"
#include "message.h"

namespace mongo {

    class MessagingPort;
    class PiggyBackData;

    typedef AtomicUInt MSGID;

    class AbstractMessagingPort : boost::noncopyable {
    public:
        AbstractMessagingPort() : tag(0) {}
        virtual ~AbstractMessagingPort() { }
        virtual void reply(Message& received, Message& response, MSGID responseTo) = 0; // like the reply below, but doesn't rely on received.data still being available
        virtual void reply(Message& received, Message& response) = 0;

        virtual HostAndPort remote() const = 0;
        virtual unsigned remotePort() const = 0;

    private:

    public:
        // TODO make this private with some helpers

        /* ports can be tagged with various classes.  see closeAllSockets(tag). defaults to 0. */
        unsigned tag;

    };

    class MessagingPort : public AbstractMessagingPort {
    public:
        MessagingPort(int sock, const SockAddr& farEnd);

        // in some cases the timeout will actually be 2x this value - eg we do a partial send,
        // then the timeout fires, then we try to send again, then the timeout fires again with
        // no data sent, then we detect that the other side is down
        MessagingPort(double so_timeout = 0, int logLevel = 0 );

        virtual ~MessagingPort();

        void shutdown();

        bool connect(SockAddr& farEnd);

        /* it's assumed if you reuse a message object, that it doesn't cross MessagingPort's.
           also, the Message data will go out of scope on the subsequent recv call.
        */
        bool recv(Message& m);
        void reply(Message& received, Message& response, MSGID responseTo);
        void reply(Message& received, Message& response);
        bool call(Message& toSend, Message& response);

        void say(Message& toSend, int responseTo = -1);

        /**
         * this is used for doing 'async' queries
         * instead of doing call( to , from )
         * you would do
         * say( to )
         * recv( from )
         * Note: if you fail to call recv and someone else uses this port,
         *       horrible things will happend
         */
        bool recv( const Message& sent , Message& response );

        void piggyBack( Message& toSend , int responseTo = -1 );

        virtual unsigned remotePort() const;
        virtual HostAndPort remote() const;

        // send len or throw SocketException
        void send( const char * data , int len, const char *context );
        void send( const vector< pair< char *, int > > &data, const char *context );

        // recv len or throw SocketException
        void recv( char * data , int len );

        int unsafe_recv( char *buf, int max );

        void clearCounters() { _bytesIn = 0; _bytesOut = 0; }
        long long getBytesIn() const { return _bytesIn; }
        long long getBytesOut() const { return _bytesOut; }
    private:
        int sock;
        PiggyBackData * piggyBackData;

        long long _bytesIn;
        long long _bytesOut;
        
        // this is the parsed version of farEnd
        // mutable because its initialized only on call to remote()
        mutable HostAndPort _farEndParsed; 

    public:
        SockAddr farEnd;
        double _timeout;
        int _logLevel; // passed to log() when logging errors

        static void closeAllSockets(unsigned tagMask = 0xffffffff);

        friend class PiggyBackData;
    };


} // namespace mongo
