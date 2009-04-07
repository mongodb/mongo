// request.h

#pragma once

#include "../stdafx.h"
#include "../util/message.h"
#include "../db/dbmessage.h"
#include "config.h"

namespace mongo {
    
    class Request : boost::noncopyable {
    public:
        Request( Message& m, AbstractMessagingPort* p );

        // ---- message info -----


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
        
        ShardManager * getShardManager(){
            return _shardInfo;
        }

        // ---- remote location info -----

        
        string singleServerName();
        
        const char * primaryName(){
            return _config->getPrimary().c_str();
        }

        // ---- low level access ----

        void reply( Message & response ){
            _p->reply( _m , response , _id );
        }
        
        Message& m(){ return _m; }
        DbMessage& d(){ return _d; }
        AbstractMessagingPort* p(){ return _p; }

        void process( int attempt = 0 );
        
    private:
        
        void reset( bool reload=true );
        
        Message& _m;
        DbMessage _d;
        AbstractMessagingPort* _p;
        
        MSGID _id;
        DBConfig * _config;
        ShardManager * _shardInfo;
    };
    
    class StaleConfigException : public std::exception {
        
    };
}

#include "strategy.h"
