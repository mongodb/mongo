// grid.cpp

/**
*    Copyright (C) 2010 10gen Inc.
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

#include <iomanip>

#include "../client/connpool.h"
#include "../util/stringutils.h"

#include "grid.h"
#include "shard.h"

namespace mongo {
    
    DBConfigPtr Grid::getDBConfig( string database , bool create , const string& shardNameHint ){
        {
            string::size_type i = database.find( "." );
            if ( i != string::npos )
                database = database.substr( 0 , i );
        }
        
        if ( database == "config" )
            return configServerPtr;

        scoped_lock l( _lock );

        DBConfigPtr& cc = _databases[database];
        if ( !cc ){
            cc.reset(new DBConfig( database ));
            if ( ! cc->load() ){
                if ( create ){
                    // note here that cc->primary == 0.
                    log() << "couldn't find database [" << database << "] in config db" << endl;
                    
                    { // lets check case
                        ScopedDbConnection conn( configServer.modelServer() );
                        BSONObjBuilder b;
                        b.appendRegex( "_id" , (string)"^" + database + "$" , "i" );
                        BSONObj d = conn->findOne( ShardNS::database , b.obj() );
                        conn.done();

                        if ( ! d.isEmpty() ){
                            cc.reset();
                            stringstream ss;
                            ss <<  "can't have 2 databases that just differ on case " 
                               << " have: " << d["_id"].String()
                               << " want to add: " << database;

                            uasserted( DatabaseDifferCaseCode ,ss.str() );
                        }
                    }

                    Shard primary;
                    if ( database == "admin" ){
                        primary = configServer.getPrimary();

                    } else if ( shardNameHint.empty() ){
                        primary = Shard::pick();

                    } else {
                        // use the shard name if provided
                        Shard shard;
                        shard.reset( shardNameHint );
                        primary = shard;
                    }

                    if ( primary.ok() ){
                        cc->setPrimary( primary.getName() ); // saves 'cc' to configDB
                        log() << "\t put [" << database << "] on: " << primary << endl;
                    }
                    else {
                        cc.reset();
                        log() << "\t can't find a shard to put new db on" << endl;
                        uasserted( 10185 ,  "can't find a shard to put new db on" );
                    }
                }
                else {
                    cc.reset();
                }
            }
            
        }
        
        return cc;
    }

    void Grid::removeDB( string database ){
        uassert( 10186 ,  "removeDB expects db name" , database.find( '.' ) == string::npos );
        scoped_lock l( _lock );
        _databases.erase( database );
        
    }

    bool Grid::allowLocalHost() const {
        return _allowLocalShard;
    }

    void Grid::setAllowLocalHost( bool allow ){
        _allowLocalShard = allow;
    }

    bool Grid::addShard( string* name , const ConnectionString& servers , long long maxSize , string& errMsg ){
        // name can be NULL, so privide a dummy one here to avoid testing it elsewhere
        string nameInternal;
        if ( ! name ) {
            name = &nameInternal;
        }

        // Check whether the host (or set) exists and run several sanity checks on this request. 

        vector<string> dbNames;
        try {
            ScopedDbConnection newShardConn( servers );
            newShardConn->getLastError();
            
            if ( newShardConn->type() == ConnectionString::SYNC ){
                newShardConn.done();
                errMsg = "can't use sync cluster as a shard.  for replica set, have to use <setname>/<server1>,<server2>,...";
                return false;
            }

            BSONObj resIsMaster;
            bool ok =  newShardConn->runCommand( "admin" , BSON( "isMaster" << 1 ) , resIsMaster );
            if ( !ok ){
                ostringstream ss;
                ss << "failed running isMaster: " << resIsMaster;
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            // if the shard has only one host, make sure it is not part of a replica set
            string setName = resIsMaster["setName"].str();
            string commandSetName = servers.getSetName();
            if ( commandSetName.empty() && ! setName.empty() ){
                ostringstream ss;
                ss << "host is part of set: " << setName << " use replica set url format <setname>/<server1>,<server2>,....";
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            // if the shard is part of replica set, make sure it is the right one
            if ( ! commandSetName.empty() && ( commandSetName != setName ) ){
                ostringstream ss;
                ss << "host is part of a different set: " << setName;
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            // TODO Check that the hosts in 'server' all belong to the replica set 

            // shard name defaults to the name of the replica set
            if ( name->empty() && ! setName.empty() )
                    *name = setName;

            // In order to be accepted as a new shard, that mongod must not have any database name that exists already 
            // in any other shards. If that test passes, the new shard's databases are going to be entered as 
            // non-sharded db's whose primary is the newly added shard.

            BSONObj resListDB;
            ok = newShardConn->runCommand( "admin" , BSON( "listDatabases" << 1 ) , resListDB );
            if ( !ok ){
                ostringstream ss;
                ss << "failed listing " << servers.toString() << "'s databases:" << resListDB;
                errMsg = ss.str();
                newShardConn.done();
                return false;
            }

            BSONObjIterator i( resListDB["databases"].Obj() );
            while ( i.more() ){
                BSONObj dbEntry = i.next().Obj();
                const string& dbName = dbEntry["name"].String();
                if ( _isSpecialLocalDB( dbName ) ){
                    // 'local', 'admin', and 'config' are system DBs and should be excluded here
                    continue;
                } else {
                    dbNames.push_back( dbName );
                }
            }

            newShardConn.done();
        }
        catch ( DBException& e ){
            ostringstream ss;
            ss << "couldn't connect to new shard ";
            ss << e.what();
            errMsg = ss.str();
            return false;
        }

        // check that none of the existing shard candidate's db's exist elsewhere
        for ( vector<string>::const_iterator it = dbNames.begin(); it != dbNames.end(); ++it ){
            DBConfigPtr config = getDBConfig( *it , false );
            if ( config.get() != NULL ){
                ostringstream ss;
                ss << "trying to add shard " << servers.toString() << " because local database " << *it;
                ss << " exists in another " << config->getPrimary().toString();
                errMsg = ss.str();
                return false;
            }
        }

        // if a name for a shard wasn't provided, pick one.
        if ( name->empty() && ! _getNewShardName( name ) ){
            errMsg = "error generating new shard name";
            return false;
        }
            
        // build the ConfigDB shard document
        BSONObjBuilder b;
        b.append( "_id" , *name );
        b.append( "host" , servers.toString() );
        if ( maxSize > 0 ){
            b.append( ShardFields::maxSize.name() , maxSize );
        }
        BSONObj shardDoc = b.obj();

        {
            ScopedDbConnection conn( configServer.getPrimary() );
                
            // check whether the set of hosts (or single host) is not an already a known shard
            BSONObj old = conn->findOne( ShardNS::shard , BSON( "host" << servers.toString() ) );
            if ( ! old.isEmpty() ){
                errMsg = "host already used";
                conn.done();
                return false;
            }

            log() << "going to add shard: " << shardDoc << endl;

            conn->insert( ShardNS::shard , shardDoc );
            errMsg = conn->getLastError();
            if ( ! errMsg.empty() ){
                log() << "error adding shard: " << shardDoc << " err: " << errMsg << endl;
                conn.done();
                return false;
            }

            conn.done();
        }

        Shard::reloadShardInfo();

        // add all databases of the new shard
        for ( vector<string>::const_iterator it = dbNames.begin(); it != dbNames.end(); ++it ){
            DBConfigPtr config = getDBConfig( *it , true , *name );
            if ( ! config ){
                log() << "adding shard " << servers << " even though could not add database " << *it << endl; 
            }
        }

        return true;
    }
        
    bool Grid::knowAboutShard( const string& name ) const{
        ShardConnection conn( configServer.getPrimary() , "" );
        BSONObj shard = conn->findOne( ShardNS::shard , BSON( "host" << name ) );
        conn.done();
        return ! shard.isEmpty();
    }

    bool Grid::_getNewShardName( string* name ) const{
        DEV assert( name );

        bool ok = false;
        int count = 0; 

        ShardConnection conn( configServer.getPrimary() , "" );
        BSONObj o = conn->findOne( ShardNS::shard , Query( fromjson ( "{_id: /^shard/}" ) ).sort(  BSON( "_id" << -1 ) ) ); 
        if ( ! o.isEmpty() ) {
            string last = o["_id"].String();
            istringstream is( last.substr( 5 ) );
            is >> count;
            count++;
        }                                                                                                               
        if (count < 9999) {
            stringstream ss;
            ss << "shard" << setfill('0') << setw(4) << count;
            *name = ss.str();
            ok = true;
        }
        conn.done();

        return ok;
    }

    bool Grid::shouldBalance() const {
        ShardConnection conn( configServer.getPrimary() , "" );

        // look for the stop balancer marker
        BSONObj stopMarker = conn->findOne( ShardNS::settings, BSON( "_id" << "balancer" << "stopped" << true ) );
        conn.done();
        return stopMarker.isEmpty();
    }

    unsigned long long Grid::getNextOpTime() const {
        ScopedDbConnection conn( configServer.getPrimary() );
        
        BSONObj result;
        massert( 10421 ,  "getoptime failed" , conn->simpleCommand( "admin" , &result , "getoptime" ) );
        conn.done();

        return result["optime"]._numberLong();
    }

    bool Grid::_isSpecialLocalDB( const string& dbName ){
        return ( dbName == "local" ) || ( dbName == "admin" ) || ( dbName == "config" );
    }

    Grid grid;

} 
