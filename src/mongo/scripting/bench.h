/*
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

#pragma once

#include <string>

#include <boost/shared_ptr.hpp>
#include <pcrecpp.h>

#include "mongo/bson/util/atomic_int.h"
#include "mongo/client/connpool.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

    struct BenchRunConfig : private boost::noncopyable {
        BenchRunConfig();

        std::string host;
        std::string db;
        std::string username;
        std::string password;

        unsigned parallel;
        double seconds;

        bool hideResults;
        bool handleErrors;
        bool hideErrors;

        boost::shared_ptr< pcrecpp::RE > trapPattern;
        boost::shared_ptr< pcrecpp::RE > noTrapPattern;
        boost::shared_ptr< pcrecpp::RE > watchPattern;
        boost::shared_ptr< pcrecpp::RE > noWatchPattern;

        BSONObj ops;

        volatile bool active; // true at starts, gets set to false when should stop
        AtomicUInt threadsReady;

        bool error;
        bool throwGLE;
        bool breakOnTrap;
        bool loopCommands;

        AtomicUInt threadsActive;

        mongo::mutex _mutex;
        long long errCount;
        BSONArrayBuilder trapped;
    };

    struct BenchRunStats : private boost::noncopyable {
        BenchRunStats();

        unsigned long long findOneTotalTimeMicros;
        unsigned long long updateTotalTimeMicros;
        unsigned long long insertTotalTimeMicros;
        unsigned long long deleteTotalTimeMicros;
        unsigned long long queryTotalTimeMicros;

        unsigned long long findOneTotalOps;
        unsigned long long updateTotalOps;
        unsigned long long insertTotalOps;
        unsigned long long deleteTotalOps;
        unsigned long long queryTotalOps;

        mongo::mutex _mutex;
    };

    class BenchRunner : private boost::noncopyable {
    public:

        BenchRunner();
        ~BenchRunner();

        void init( BSONObj& args );

        void done();

        BSONObj status();

        static BenchRunner* get( BSONObj args );

        static BenchRunner* get( OID oid );

        static BSONObj finish( BenchRunner* runner );

        static map< OID, BenchRunner* > activeRuns;

        OID oid;
        BenchRunConfig config;
        BenchRunStats stats;
        vector< boost::shared_ptr< boost::thread > > threads;

        boost::shared_ptr< ScopedDbConnection > conn;
        BSONObj before;
        BSONObj after;

    };

}  // namespace mongo
