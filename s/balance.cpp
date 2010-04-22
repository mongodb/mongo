// balance.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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
*/

#include "../stdafx.h"

#include "../db/jsobj.h"
#include "../db/cmdline.h"

#include "balance.h"
#include "server.h"
#include "shard.h"
#include "config.h"

namespace mongo {
    
    Balancer balancer;

    Balancer::Balancer(){
    }

    bool Balancer::shouldIBalance( DBClientBase& conn ){
        BSONObj x = conn.findOne( ShardNS::settings , BSON( "_id" << "balancer" ) );
        log(3) << "balancer: " << x << endl;
        
        if ( ! x.isEmpty() ){
            if ( x["who"].String() == _myid ){
                log(3) << "balancer: i'm the current balancer" << endl;
                return true;
            }
            
            BSONObj other = conn.findOne( ShardNS::mongos , x["who"].wrap( "_id" ) );
            massert( 13125 , (string)"can't find mongos: " + x["who"].String() , ! other.isEmpty() );

            int secsSincePing = (int)(( jsTime() - other["ping"].Date() ) / 1000 );
            log(3) << "current balancer is: " << other << " ping delay(secs): " << secsSincePing << endl;
            
            if ( secsSincePing < ( 60 * 10 ) ){
                return false;
            }
            
            log(3) << "balancer: going to take over" << endl;
            // we want to take over, so fall through to below
        }
        
        OID hack;
        hack.init();

        conn.update( ShardNS::settings , 
                     BSON( "_id" << "balancer" ) , 
                     BSON( "$set" << BSON( "who" << _myid << "x" << hack ) ) ,
                     true );
        
        x = conn.findOne( ShardNS::settings , BSON( "_id" << "balancer" ) );
        log(3) << "balancer: after update: " << x << endl;
        return _myid == x["who"].String() && hack == x["x"].OID();
    }
    
    void Balancer::run(){

        { // init stuff, don't want to do at static init
            StringBuilder buf;
            buf << ourHostname << ":" << cmdLine.port;
            _myid = buf.str();
            log(1) << "balancer myid: " << _myid << endl;
            
            _started = time(0);
        }


        while ( ! inShutdown() ){
            sleepsecs( 30 );
            
            try {
                ShardConnection conn( configServer.getPrimary() );
                conn->update( ShardNS::mongos , 
                              BSON( "_id" << _myid ) , 
                              BSON( "$set" << BSON( "ping" << DATENOW << "up" << (int)(time(0)-_started) ) ) , 
                              true );
                
                if ( shouldIBalance( conn.conn() ) ){
                    log() << "i'm going to do some balancing" << endl;
                }
                
                conn.done();
            }
            catch ( std::exception& e ){
                log() << "caught exception while doing mongos ping: " << e.what() << endl;
                continue;
            }
            
        }
    }

}
