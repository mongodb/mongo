// request.h

#pragma once

#include "../stdafx.h"
#include "../util/message.h"
#include "../db/dbmessage.h"
#include "config.h"

namespace mongo {
    
    class Request {
    public:
        Request( Message& m, MessagingPort& p );

        const char * getns(){
            return _d.getns();
        }
        int op(){
            return _m.data->operation();
        }
        bool expectResponse(){
            return op() == dbQuery || op() == dbGetMore;
        }
        
        MSGID id(){
            return _id;
        }

        DBConfig * getConfig(){
            return _config;
        }

        const char * primaryName(){
            return _config->getPrimary().c_str();
        }

        string singleServerName(){
            string s = _config->getServer( getns() );
            uassert( "sharded!" , s.size() > 0 );
            return s;
        }

        void reply( Message & response ){
            _p.reply( _m , response , _id );
        }
        
        Message& m(){ return _m; }
        DbMessage& d(){ return _d; }
        MessagingPort& p(){ return _p; }

        void process();

    private:
        Message& _m;
        DbMessage _d;
        MessagingPort& _p;

        MSGID _id;
        DBConfig * _config;
    };

    class Strategy {
    public:
        virtual ~Strategy() {}
        virtual void queryOp( Request& r ) = 0;
        virtual void getMore( Request& r ) = 0;
        virtual void writeOp( int op , Request& r ) = 0;
    };
    
    extern Strategy * SINGLE;
    extern Strategy * RANDOM;
}
