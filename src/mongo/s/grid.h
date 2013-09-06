// grid.h

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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#pragma once

#include <boost/date_time/posix_time/posix_time.hpp>

#include "mongo/util/time_support.h"
#include "mongo/util/concurrency/mutex.h"

#include "config.h"  // DBConfigPtr

namespace mongo {

    /**
     * stores meta-information about the grid
     * TODO: used shard_ptr for DBConfig pointers
     */
    class Grid {
    public:
        Grid() : _lock( "Grid" ) , _allowLocalShard( true ) { }

        /**
         * gets the config the db.
         * will return an empty DBConfig if not in db already
         */
        DBConfigPtr getDBConfig( const StringData& ns , bool create=true , const string& shardNameHint="" );

        /**
         * removes db entry.
         * on next getDBConfig call will fetch from db
         */
        void removeDB( const std::string& db );

        /**
         * removes db entry - only this DBConfig object will be removed,
         *  other DBConfigs which may have been created in the meantime will not be harmed
         *  on next getDBConfig call will fetch from db
         *
         *  Using this method avoids race conditions where multiple threads detect a database
         *  reload has failed.
         *
         *  Example : N threads receive version exceptions and dbConfig.reload(), while
         *  simultaneously a dropDatabase is occurring.  In the meantime, the dropDatabase call
         *  attempts to create a DBConfig object if one does not exist, to load the db info,
         *  but the config is repeatedly deleted as soon as it is created.  Using this method
         *  prevents the deletion of configs we don't know about.
         *
         */
        void removeDBIfExists( const DBConfig& database );

        /**
         * @return true if shards and config servers are allowed to use 'localhost' in address
         */
        bool allowLocalHost() const;

        /**
         * @param whether to allow shards and config servers to use 'localhost' in address
         */
        void setAllowLocalHost( bool allow );

        /**
         *
         * addShard will create a new shard in the grid. It expects a mongod process to be running
         * on the provided address. Adding a shard that is a replica set is supported.
         *
         * @param name is an optional string with the name of the shard. if omitted, grid will
         *        generate one and update the parameter.
         * @param servers is the connection string of the shard being added
         * @param maxSize is the optional space quota in bytes. Zeros means there's no limitation to
         *        space usage
         * @param errMsg is the error description in case the operation failed.
         * @return true if shard was successfully added.
         */
        bool addShard( string* name , const ConnectionString& servers , long long maxSize , string& errMsg );

        /**
         * @return true if the config database knows about a host 'name'
         */
        bool knowAboutShard( const string& name ) const;

        /**
         * @return true if the chunk balancing functionality is enabled
         */
        bool shouldBalance( const string& ns = "", BSONObj* balancerDocOut = 0 ) const;

        /**
         * 
         * Obtain grid configuration and settings data.
         *
         * @param name identifies a particular type of configuration data.
         * @return a BSON object containing the requested data.
         */
        BSONObj getConfigSetting( const std::string& name ) const;

        unsigned long long getNextOpTime() const;
        
        void flushConfig();

        // exposed methods below are for testing only

        /**
         * @param balancerDoc bson that may contain a window of time for the balancer to work
         *        format { ... , activeWindow: { start: "8:30" , stop: "19:00" } , ... }
         * @return true if there is no window of time specified for the balancer or it we're currently in it
         */
        static bool _inBalancingWindow( const BSONObj& balancerDoc , const boost::posix_time::ptime& now );

    private:
        mongo::mutex              _lock;            // protects _databases; TODO: change to r/w lock ??
        map<string, DBConfigPtr > _databases;       // maps ns to DBConfig's
        bool                      _allowLocalShard; // can 'localhost' be used in shard addresses?

        /**
         * @param name is the chose name for the shard. Parameter is mandatory.
         * @return true if it managed to generate a shard name. May return false if (currently)
         * 10000 shard
         */
        bool _getNewShardName( string* name ) const;

        /**
         * @return whether a give dbname is used for shard "local" databases (e.g., admin or local)
         */
        static bool _isSpecialLocalDB( const string& dbName );

        /**
         * @param balancerDoc bson that may contain a marker to stop the balancer
         *        format { ... , stopped: [ "true" | "false" ] , ... }
         * @return true if the marker is present and is set to true
         */
        static bool _balancerStopped( const BSONObj& balancerDoc );

    };

    extern Grid grid;

} // namespace mongo
