// request.h

#pragma once

#include "stdafx.h"
#include "../util/message.h"
#include "../db/dbmessage.h"

namespace mongo {
    
    class Request {
    public:
        Request( Message& m, MessagingPort& p ) : _m(m) , _d( m ) , _p(p){
            assert( _d.getns() );
            _id = _m.data->id;
        }

        const char * getns(){
            return _d.getns();
        }
        
        MSGID id(){
            return _id;
        }

        void reply( Message & response ){
            _p.reply( _m , response , _id );
        }
        
        Message& m(){ return _m; }
        DbMessage& d(){ return _d; }
        MessagingPort& p(){ return _p; }

    private:
        Message& _m;
        DbMessage _d;
        MessagingPort& _p;

        MSGID _id;
    };

    class Strategy {
    public:
        virtual void queryOp( Request& r ) = 0;
        virtual void getMore( Request& r ) = 0;
        virtual void writeOp( int op , Request& r ) = 0;
    };
    
    extern Strategy * SINGLE;
    extern Strategy * RANDOM;
}
