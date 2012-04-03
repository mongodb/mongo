// config_migrate.cpp

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

#include "pch.h"
#include "../util/net/message.h"
#include "../client/connpool.h"
#include "../client/model.h"
#include "mongo/client/dbclientcursor.h"
#include "../db/pdfile.h"
#include "../db/cmdline.h"

#include "server.h"
#include "config.h"
#include "chunk.h"

namespace mongo {

    int ConfigServer::checkConfigVersion( bool upgrade ) {
        int cur = dbConfigVersion();
        if ( cur == VERSION )
            return 0;

        if ( cur == 0 ) {
            ScopedDbConnection conn( _primary );

            // If the cluster has not previously been initialized, we need to set the version before using so
            // subsequent mongoses use the config data the same way.  This requires all three config servers online
            // initially.
            try {
                conn->insert( "config.version" , BSON( "_id" << 1 << "version" << VERSION ) );
            }
            catch( DBException& ){
                error() << "All config servers must initially be reachable for the cluster to be initialized." << endl;
                throw;
            }

            pool.flush();
            verify( VERSION == dbConfigVersion( conn.conn() ) );
            conn.done();
            return 0;
        }

        if ( cur == 2 ) {

            // need to upgrade
            verify( VERSION == 3 );
            if ( ! upgrade ) {
                log() << "newer version of mongo meta data\n"
                      << "need to --upgrade after shutting all mongos down"
                      << endl;
                return -9;
            }

            ScopedDbConnection conn( _primary );

            // do a backup
            string backupName;
            {
                stringstream ss;
                ss << "config-backup-" << terseCurrentTime(false);
                backupName = ss.str();
            }
            log() << "backing up config to: " << backupName << endl;
            conn->copyDatabase( "config" , backupName );

            map<string,string> hostToShard;
            set<string> shards;
            // shards
            {
                unsigned n = 0;
                auto_ptr<DBClientCursor> c = conn->query( ShardNS::shard , BSONObj() );
                while ( c->more() ) {
                    BSONObj o = c->next();
                    string host = o["host"].String();

                    string name = "";

                    BSONElement id = o["_id"];
                    if ( id.type() == String ) {
                        name = id.String();
                    }
                    else {
                        stringstream ss;
                        ss << "shard" << hostToShard.size();
                        name = ss.str();
                    }

                    hostToShard[host] = name;
                    shards.insert( name );
                    n++;
                }

                verify( n == hostToShard.size() );
                verify( n == shards.size() );

                conn->remove( ShardNS::shard , BSONObj() );

                for ( map<string,string>::iterator i=hostToShard.begin(); i != hostToShard.end(); i++ ) {
                    conn->insert( ShardNS::shard , BSON( "_id" << i->second << "host" << i->first ) );
                }
            }

            // databases
            {
                auto_ptr<DBClientCursor> c = conn->query( ShardNS::database , BSONObj() );
                map<string,BSONObj> newDBs;
                unsigned n = 0;
                while ( c->more() ) {
                    BSONObj old = c->next();
                    n++;

                    if ( old["name"].eoo() ) {
                        // already done
                        newDBs[old["_id"].String()] = old;
                        continue;
                    }

                    BSONObjBuilder b(old.objsize());
                    b.appendAs( old["name"] , "_id" );

                    BSONObjIterator i(old);
                    while ( i.more() ) {
                        BSONElement e = i.next();
                        if ( strcmp( "_id" , e.fieldName() ) == 0 ||
                                strcmp( "name" , e.fieldName() ) == 0 ) {
                            continue;
                        }

                        b.append( e );
                    }

                    BSONObj x = b.obj();
                    log() << old << "\n\t" << x << endl;
                    newDBs[old["name"].String()] = x;
                }

                verify( n == newDBs.size() );

                conn->remove( ShardNS::database , BSONObj() );

                for ( map<string,BSONObj>::iterator i=newDBs.begin(); i!=newDBs.end(); i++ ) {
                    conn->insert( ShardNS::database , i->second );
                }

            }

            // chunks
            {
                unsigned num = 0;
                map<string,BSONObj> chunks;
                auto_ptr<DBClientCursor> c = conn->query( ShardNS::chunk , BSONObj() );
                while ( c->more() ) {
                    BSONObj x = c->next();
                    BSONObjBuilder b;

                    string id = Chunk::genID( x["ns"].String() , x["min"].Obj() );
                    b.append( "_id" , id );

                    BSONObjIterator i(x);
                    while ( i.more() ) {
                        BSONElement e = i.next();
                        if ( strcmp( e.fieldName() , "_id" ) == 0 )
                            continue;
                        b.append( e );
                    }

                    BSONObj n = b.obj();
                    log() << x << "\n\t" << n << endl;
                    chunks[id] = n;
                    num++;
                }

                verify( num == chunks.size() );

                conn->remove( ShardNS::chunk , BSONObj() );
                for ( map<string,BSONObj>::iterator i=chunks.begin(); i!=chunks.end(); i++ ) {
                    conn->insert( ShardNS::chunk , i->second );
                }

            }

            conn->update( "config.version" , BSONObj() , BSON( "_id" << 1 << "version" << VERSION ) );
            conn.done();
            pool.flush();
            return 1;
        }

        log() << "don't know how to upgrade " << cur << " to " << VERSION << endl;
        return -8;
    }

}
