// shard.cpp

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

#include "stdafx.h"
#include "shard.h"
#include "config.h"

namespace mongo {
    
    string ShardInfo::modelServer() {
        // TODO: this could move around?
        return configServer.modelServer();
    }

    void ShardInfo::serialize(BSONObjBuilder& to) {
        to.append( "ns", _ns );
        
    }

    void ShardInfo::unserialize(BSONObj& from) {
        _ns = from.getStringField("ns");
        uassert("bad config.shards.name", !_ns.empty());

        _shards.clear();
    }

    void shardObjTest(){
        string ns = "alleyinsider.blog.posts";
        BSONObj o = BSON( "ns" << ns );

        ShardInfo si;
        si.unserialize( o );
        assert( si.getns() == ns );
        
        

        BSONObjBuilder b;
        si.serialize( b );
        BSONObj a = b.obj();
        assert( ns == a.getStringField( "ns" ) );

        log(1) << "shardObjTest passed" << endl;
    }

} // namespace mongo
