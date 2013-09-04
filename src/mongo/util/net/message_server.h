// message_server.h

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

/*
  abstract database server
  async io core, worker thread system
 */

#pragma once

#include "mongo/pch.h"

namespace mongo {

    struct LastError;

    class MessageHandler {
    public:
        virtual ~MessageHandler() {}
        
        /**
         * called once when a socket is connected
         */
        virtual void connected( AbstractMessagingPort* p ) = 0;

        /**
         * called every time a message comes in
         * handler is responsible for responding to client
         */
        virtual void process( Message& m , AbstractMessagingPort* p , LastError * err ) = 0;

        /**
         * called once when a socket is disconnected
         */
        virtual void disconnected( AbstractMessagingPort* p ) = 0;
    };

    class MessageServer {
    public:
        struct Options {
            int port;                   // port to bind to
            string ipList;             // addresses to bind to

            Options() : port(0), ipList("") {}
        };

        virtual ~MessageServer() {}
        virtual void run() = 0;
        virtual void setAsTimeTracker() = 0;
        virtual void setupSockets() = 0;
    };

    // TODO use a factory here to decide between port and asio variations
    MessageServer * createServer( const MessageServer::Options& opts , MessageHandler * handler );
}
