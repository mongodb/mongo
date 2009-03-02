// message_server.h

/*
  abstract database server
  async io core, worker thread system
 */

#pragma once

#include "stdafx.h"

namespace mongo {
    
    class MessageHandler {
    public:
        virtual ~MessageHandler(){}
        virtual void process( Message& m , AbstractMessagingPort* p ) = 0;
    };
    
    class MessageServer {
    public:
        MessageServer( int port , MessageHandler * handler ) : _port( port ) , _handler( handler ){}
        virtual ~MessageServer(){}

        virtual void run() = 0;
        
    protected:
        
        int _port;
        MessageHandler* _handler;
    };

    MessageServer * createServer( int port , MessageHandler * handler );
}
