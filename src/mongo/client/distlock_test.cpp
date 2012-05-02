// distlock_test.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <iostream>
#include "pch.h"
#include "distlock.h"
#include "../db/commands.h"
#include "../util/bson_util.h"
#include "../util/timer.h"

// Modify some config options for the RNG, since they cause MSVC to fail
#include <boost/config.hpp>

#if defined(BOOST_MSVC) && defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
#undef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
#define BOOST_RNG_HACK
#endif

// Well, sort-of cross-platform RNG
#include <boost/random/mersenne_twister.hpp>

#ifdef BOOST_RNG_HACK
#define BOOST_NO_MEMBER_TEMPLATE_FRIENDS
#undef BOOST_RNG_HACK
#endif


#include <boost/random/uniform_int.hpp>
#include <boost/random/variate_generator.hpp>


// TODO:  Make a method in BSONObj if useful, don't modify for now
#define string_field(obj, name, def) ( obj.hasField(name) ? obj[name].String() : def )
#define number_field(obj, name, def) ( obj.hasField(name) ? obj[name].Number() : def )

namespace mongo {

    class TestDistLockWithSync: public Command {
    public:
        TestDistLockWithSync() :
            Command("_testDistLockWithSyncCluster") {
        }
        virtual void help(stringstream& help) const {
            help << "should not be calling this directly" << endl;
        }

        virtual bool slaveOk() const {
            return false;
        }
        virtual bool adminOnly() const {
            return true;
        }
        virtual LockType locktype() const {
            return NONE;
        }

        static void runThread() {
            while (keepGoing) {
                if (current->lock_try( "test" )) {
                    count++;
                    int before = count;
                    sleepmillis(3);
                    int after = count;

                    if (after != before) {
                        error() << " before: " << before << " after: " << after
                                << endl;
                    }

                    current->unlock();
                }
            }
        }

        bool run(const string&, BSONObj& cmdObj, int, string& errmsg,
                 BSONObjBuilder& result, bool) {
            Timer t;
            DistributedLock lk(ConnectionString(cmdObj["host"].String(),
                                                ConnectionString::SYNC), "testdistlockwithsync", 0, 0);
            current = &lk;
            count = 0;
            gotit = 0;
            errors = 0;
            keepGoing = true;

            vector<shared_ptr<boost::thread> > l;
            for (int i = 0; i < 4; i++) {
                l.push_back(
                    shared_ptr<boost::thread> (new boost::thread(runThread)));
            }

            int secs = 10;
            if (cmdObj["secs"].isNumber())
                secs = cmdObj["secs"].numberInt();
            sleepsecs(secs);
            keepGoing = false;

            for (unsigned i = 0; i < l.size(); i++)
                l[i]->join();

            current = 0;

            result.append("count", count);
            result.append("gotit", gotit);
            result.append("errors", errors);
            result.append("timeMS", t.millis());

            return errors == 0;
        }

        // variables for test
        static DistributedLock * current;
        static int gotit;
        static int errors;
        static AtomicUInt count;

        static bool keepGoing;

    } testDistLockWithSyncCmd;

    DistributedLock * TestDistLockWithSync::current;
    AtomicUInt TestDistLockWithSync::count;
    int TestDistLockWithSync::gotit;
    int TestDistLockWithSync::errors;
    bool TestDistLockWithSync::keepGoing;



    class TestDistLockWithSkew: public Command {
    public:

        static const int logLvl = 1;

        TestDistLockWithSkew() :
            Command("_testDistLockWithSkew") {
        }
        virtual void help(stringstream& help) const {
            help << "should not be calling this directly" << endl;
        }

        virtual bool slaveOk() const {
            return false;
        }
        virtual bool adminOnly() const {
            return true;
        }
        virtual LockType locktype() const {
            return NONE;
        }

        void runThread(ConnectionString& hostConn, unsigned threadId, unsigned seed,
                       BSONObj& cmdObj, BSONObjBuilder& result) {

            stringstream ss;
            ss << "thread-" << threadId;
            setThreadName(ss.str().c_str());

            // Lock name
            string lockName = string_field(cmdObj, "lockName", this->name + "_lock");

            // Range of clock skew in diff threads
            int skewRange = (int) number_field(cmdObj, "skewRange", 1);

            // How long to wait with the lock
            int threadWait = (int) number_field(cmdObj, "threadWait", 30);
            if(threadWait <= 0) threadWait = 1;

            // Max amount of time (ms) a thread waits before checking the lock again
            int threadSleep = (int) number_field(cmdObj, "threadSleep", 30);
            if(threadSleep <= 0) threadSleep = 1;

            // How long until the lock is forced in ms, only compared locally
            unsigned long long takeoverMS = (unsigned long long) number_field(cmdObj, "takeoverMS", 0);

            // Whether or not we should hang some threads
            int hangThreads = (int) number_field(cmdObj, "hangThreads", 0);


            boost::mt19937 gen((boost::mt19937::result_type) seed);

            boost::variate_generator<boost::mt19937&, boost::uniform_int<> > randomSkew(gen, boost::uniform_int<>(0, skewRange));
            boost::variate_generator<boost::mt19937&, boost::uniform_int<> > randomWait(gen, boost::uniform_int<>(1, threadWait));
            boost::variate_generator<boost::mt19937&, boost::uniform_int<> > randomSleep(gen, boost::uniform_int<>(1, threadSleep));
            boost::variate_generator<boost::mt19937&, boost::uniform_int<> > randomNewLock(gen, boost::uniform_int<>(0, 3));


            int skew = 0;
            if (!lock.get()) {

                // Pick a skew, but the first two threads skew the whole range
                if(threadId == 0)
                    skew = -skewRange / 2;
                else if(threadId == 1)
                    skew = skewRange / 2;
                else skew = randomSkew() - (skewRange / 2);

                // Skew this thread
                jsTimeVirtualThreadSkew( skew );

                log() << "Initializing lock with skew of " << skew << " for thread " << threadId << endl;

                lock.reset(new DistributedLock(hostConn, lockName, takeoverMS, true ));

                log() << "Skewed time " << jsTime() << "  for thread " << threadId << endl
                      << "  max wait (with lock: " << threadWait << ", after lock: " << threadSleep << ")" << endl
                      << "  takeover in " << takeoverMS << "(ms remote)" << endl;

            }

            DistributedLock* myLock = lock.get();

            bool errors = false;
            BSONObj lockObj;
            while (keepGoing) {
                try {

                    if (myLock->lock_try("Testing distributed lock with skew.", false, &lockObj )) {

                        log() << "**** Locked for thread " << threadId << " with ts " << lockObj["ts"] << endl;

                        if( count % 2 == 1 && ! myLock->lock_try( "Testing lock re-entry.", true ) ) {
                            errors = true;
                            log() << "**** !Could not re-enter lock already held" << endl;
                            break;
                        }

                        if( count % 3 == 1 && myLock->lock_try( "Testing lock non-re-entry.", false ) ) {
                            errors = true;
                            log() << "**** !Invalid lock re-entry" << endl;
                            break;
                        }

                        count++;
                        int before = count;
                        int sleep = randomWait();
                        sleepmillis(sleep);
                        int after = count;

                        if(after != before) {
                            errors = true;
                            log() << "**** !Bad increment while sleeping with lock for: " << sleep << "ms" << endl;
                            break;
                        }

                        // Unlock only half the time...
                        if(hangThreads == 0 || threadId % hangThreads != 0) {
                            log() << "**** Unlocking for thread " << threadId << " with ts " << lockObj["ts"] << endl;
                            myLock->unlock( &lockObj );
                        }
                        else {
                            log() << "**** Not unlocking for thread " << threadId << endl;
                            verify( DistributedLock::killPinger( *myLock ) );
                            // We're simulating a crashed process...
                            break;
                        }
                    }

                }
                catch( LockException& e ) {
                    log() << "*** !Could not try distributed lock." << causedBy( e ) << endl;
                    break;
                }

                // Create a new lock 1/3 of the time
                if( randomNewLock() > 1 ){
                    lock.reset(new DistributedLock( hostConn, lockName, takeoverMS, true ));
                    myLock = lock.get();
                }

                sleepmillis(randomSleep());
            }

            result << "errors" << errors
                   << "skew" << skew
                   << "takeover" << (long long) takeoverMS
                   << "localTimeout" << (takeoverMS > 0);

        }

        void test(ConnectionString& hostConn, string& lockName, unsigned seed) {
            return;
        }

        bool run(const string&, BSONObj& cmdObj, int, string& errmsg,
                 BSONObjBuilder& result, bool) {

            Timer t;

            ConnectionString hostConn(cmdObj["host"].String(),
                                      ConnectionString::SYNC);

            unsigned seed = (unsigned) number_field(cmdObj, "seed", 0);
            int numThreads = (int) number_field(cmdObj, "numThreads", 4);
            int wait = (int) number_field(cmdObj, "wait", 10000);

            log() << "Starting " << this->name << " with -" << endl
                  << "  seed: " << seed << endl
                  << "  numThreads: " << numThreads << endl
                  << "  total wait: " << wait << endl << endl;

            // Skew host clocks if needed
            try {
                skewClocks( hostConn, cmdObj );
            }
            catch( DBException e ) {
                errmsg = str::stream() << "Clocks could not be skewed." << causedBy( e );
                return false;
            }

            count = 0;
            keepGoing = true;

            vector<shared_ptr<boost::thread> > threads;
            vector<shared_ptr<BSONObjBuilder> > results;
            for (int i = 0; i < numThreads; i++) {
                results.push_back(shared_ptr<BSONObjBuilder> (new BSONObjBuilder()));
                threads.push_back(shared_ptr<boost::thread> (new boost::thread(
                                      boost::bind(&TestDistLockWithSkew::runThread, this,
                                                  hostConn, (unsigned) i, seed + i, boost::ref(cmdObj),
                                                  boost::ref(*(results[i].get()))))));
            }

            sleepsecs(wait / 1000);
            keepGoing = false;

            bool errors = false;
            for (unsigned i = 0; i < threads.size(); i++) {
                threads[i]->join();
                errors = errors || results[i].get()->obj()["errors"].Bool();
            }

            result.append("count", count);
            result.append("errors", errors);
            result.append("timeMS", t.millis());

            return !errors;

        }

        /**
         * Skews the clocks of a remote cluster by a particular amount, specified by
         * the "skewHosts" element in a BSONObj.
         */
        static void skewClocks( ConnectionString& cluster, BSONObj& cmdObj ) {

            vector<long long> skew;
            if(cmdObj.hasField("skewHosts")) {
                bsonArrToNumVector<long long>(cmdObj["skewHosts"], skew);
            }
            else {
                log( logLvl ) << "No host clocks to skew." << endl;
                return;
            }

            log( logLvl ) << "Skewing clocks of hosts " << cluster << endl;

            unsigned s = 0;
            for(vector<long long>::iterator i = skew.begin(); i != skew.end(); ++i,s++) {

                ConnectionString server( cluster.getServers()[s] );
                ScopedDbConnection conn( server );

                BSONObj result;
                try {
                    bool success = conn->runCommand( string("admin"), BSON( "_skewClockCommand" << 1 << "skew" << *i ), result );

                    uassert(13678, str::stream() << "Could not communicate with server " << server.toString() << " in cluster " << cluster.toString() << " to change skew by " << *i, success );

                    log( logLvl + 1 ) << " Skewed host " << server << " clock by " << *i << endl;
                }
                catch(...) {
                    conn.done();
                    throw;
                }

                conn.done();

            }

        }

        // variables for test
        thread_specific_ptr<DistributedLock> lock;
        AtomicUInt count;
        bool keepGoing;

    } testDistLockWithSkewCmd;


    /**
     * Utility command to virtually skew the clock of a mongo server a particular amount.
     * This skews the clock globally, per-thread skew is also possible.
     */
    class SkewClockCommand: public Command {
    public:
        SkewClockCommand() :
            Command("_skewClockCommand") {
        }
        virtual void help(stringstream& help) const {
            help << "should not be calling this directly" << endl;
        }

        virtual bool slaveOk() const {
            return false;
        }
        virtual bool adminOnly() const {
            return true;
        }
        virtual LockType locktype() const {
            return NONE;
        }

        bool run(const string&, BSONObj& cmdObj, int, string& errmsg,
                 BSONObjBuilder& result, bool) {

            long long skew = (long long) number_field(cmdObj, "skew", 0);

            log() << "Adjusting jsTime() clock skew to " << skew << endl;

            jsTimeVirtualSkew( skew );

            log() << "JSTime adjusted, now is " << jsTime() << endl;

            return true;

        }

    } testSkewClockCommand;

}

