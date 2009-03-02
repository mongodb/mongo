// message_server.h

/*
  abstract database server
  async io core, worker thread system
 */

#pragma once

#include "stdafx.h"

namespace mongo {
    
    class MessageServer {
    public:
        MessageServer( int port ) : _port( port ){}
        virtual ~MessageServer(){}

        virtual void run() = 0;
        
    protected:
        int _port;
    };

    MessageServer * createServer( int port );
}
