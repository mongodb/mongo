// config.h

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

/* This file is things related to the "grid configuration":
   - what machines make up the db component of our cloud
   - where various ranges of things live
*/

#pragma once

#include "../db/namespace.h"
#include "../client/dbclient.h"
#include "../client/model.h"
#include "shardkey.h"

namespace mongo {

    class Grid;
    class ConfigServer;

    extern ConfigServer configServer;
    extern Grid grid;

    class ShardManager;
    
    /**
       top level grid configuration for an entire database
    */
    class DBConfig : public Model {
    public:
        DBConfig( string name = "" ) : _name( name ) , _primary("") , _partitioned(false){ }
        
        string getName(){ return _name; };

        /**
         * @return if anything in this db is partitioned or not
         */
        bool isPartitioned(){
            return _partitioned;
        }
        
        void turnOnPartitioning();
        ShardManager* turnOnSharding( const string& ns , ShardKeyPattern fieldsAndOrder );
        
        /**
         * @return whether or not this partition is partitioned
         */
        bool sharded( const string& ns );
        
        ShardManager* getShardManager( const string& ns , bool reload = false );

        /**
         * @return the correct for machine for the ns
         * if the namespace is partitioned, will return an empty string
         */
        string getServer( const string& ns );
        
        string getPrimary(){
            if ( _primary.size() == 0 )
                throw UserException( (string)"no primary server configured for db: " + _name );
            return _primary;
        }
        
        void setPrimary( string s ){
            _primary = s;
        }
        
        virtual void save( bool check=true);

        virtual string modelServer();
        
        // model stuff

        virtual const char * getNS(){ return "config.databases"; }
        virtual void serialize(BSONObjBuilder& to);
        virtual void unserialize(const BSONObj& from);
        bool loadByName(const char *nm);
        
    protected:
        string _name; // e.g. "alleyinsider"
        string _primary; // e.g. localhost , mongo.foo.com:9999
        bool _partitioned;
        
        map<string,ShardKeyPattern> _sharded; // { "alleyinsider.blog.posts" : { ts : 1 }  , ... ] - all ns that are sharded
        map<string,ShardManager*> _shards; // this will only have entries for things that have been looked at

        friend class Grid;
    };

    class Grid {
    public:
        /**
           gets the config the db.
           will return an empty DBConfig if not in db already
         */
        DBConfig * getDBConfig( string ns , bool create=true);
        
        string pickServerForNewDB();
        
        bool knowAboutServer( string name ) const;

        unsigned long long getNextOpTime() const;
    private:
        map<string,DBConfig*> _databases;
    };

    class ConfigServer : public DBConfig {
    public:

        enum { Port = 27016 }; /* standard port # for a grid db */
        
        ConfigServer();
        ~ConfigServer();

        bool ok(){
            // TODO: check can connect
            return _primary.size() > 0;
        }
        
        virtual string modelServer(){
            uassert( "ConfigServer not setup" , _primary.size() );
            return _primary;
        }

        /**
           call at startup, this will initiate connection to the grid db 
        */
        bool init( vector<string> configHosts , bool infer );

    private:
        string getHost( string name , bool withPort );
    };
    
} // namespace mongo
