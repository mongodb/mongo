// shard.h

/*
   A "shard" is a database (replica pair typically) which represents
   one partition of the overall database.
*/

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

#pragma once

#include "../client/dbclient.h"
#include "../client/model.h"
#include "shardkey.h"

namespace mongo {

    class ShardInfo;
    
    class Shard {
    public:
        
        BSONObj getMin(){
            return _data.getObjectField( "min" );
        }
        BSONObj getMax(){
            return _data.getObjectField( "max" );
        }
        string getServer(){
            return _data.getStringField( "server" );
        }
        
    private:
        Shard( ShardInfo * info , BSONObj data );

        ShardInfo * _info;
        BSONObj _data;

        void _split( BSONObj& middle );
        
        friend class ShardInfo;
    };

    /* config.sharding
         { ns: 'alleyinsider.fs.chunks' , 
           key: { ts : 1 } ,
           shards: [ { min: 1, max: 100, server: a } , { min: 101, max: 200 , server : b } ]
         }
    */
    class ShardInfo : public Model {
    public:

        string getns(){
            return _ns;
        }

        virtual const char * getNS() {
            return "config.sharding";
        }
        
        virtual void serialize(BSONObjBuilder& to);
        virtual void unserialize(BSONObj& from);
        virtual string modelServer();
        
    private:
        string _ns;
        ShardKey _key;
        vector<Shard> _shards;
    };
    
    void shardObjTest();

} // namespace mongo
