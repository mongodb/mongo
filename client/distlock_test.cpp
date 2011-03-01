// distlock_test.h

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
#include "../pch.h"
#include "dbclient.h"
#include "distlock.h"
#include "../db/commands.h"
#include "../util/bson_util.h"

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
                if (current->lock_try("test")) {
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

        bool run(const string&, BSONObj& cmdObj, string& errmsg,
                 BSONObjBuilder& result, bool) {
            Timer t;
            DistributedLock lk(ConnectionString(cmdObj["host"].String(),
                                                ConnectionString::SYNC), "testdistlockwithsync", 0, 0, true);
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

            // (Legacy) how long until the lock is forced in mins, measured locally
            int takeoverMins = (int) number_field(cmdObj, "takeoverMins", 0);

            // How long until the lock is forced in ms, only compared locally
            unsigned long long takeoverMS = (unsinged long long) number_field(cmdObj, "takeoverMS", 0);

            // Whether or not we should hang some threads
            int hangThreads = (int) number_field(cmdObj, "hangThreads", 0);

            int skew = 0;
            bool legacy = (takeoverMins > 0);
            if (!lock.get()) {

                // Pick a skew, but the first two threads skew the whole range
                if(threadId == 0)
                    skew = -skewRange / 2;
                else if(threadId == 1)
                    skew = skewRange / 2;
                else skew = (rand_r(&seed) % skewRange) - (skewRange / 2);

                // Skew this thread
                jsTimeVirtualThreadSkew( skew );

                log() << "Initializing lock with skew of " << skew << " for thread " << threadId << endl;

                lock.reset(new DistributedLock(hostConn, lockName, legacy ? takeoverMins : takeoverMS, true, legacy));

                log() << "Skewed time " << jsTime() << "  for thread " << threadId << endl
                      << "  max wait (with lock: " << threadWait << ", after lock: " << threadSleep << ")" << endl
                      << "  takeover in " << (legacy ? takeoverMins : takeoverMS) << (legacy ? " (mins local)" : "(ms remote)") << endl;

            }

            DistributedLock* myLock = lock.get();

            bool errors = false;
            while (keepGoing) {
        	try {

        	    if (myLock->lock_try("Testing distributed lock with skew.")) {

			log() << "**** Locked for thread " << threadId << endl;

			count++;
			int before = count;
			int sleep = (rand_r(&seed) % threadWait);
			sleepmillis(sleep);
			int after = count;

			if(after != before) {
			    errors = true;
			    log() << "**** !Bad increment while sleeping with lock for: " << sleep << "ms" << endl;
			    break;
			}

			// Unlock only half the time...
			if(hangThreads == 0 || threadId % hangThreads != 0) {
			    log() << "**** Unlocking for thread " << threadId << endl;
			    myLock->unlock();
			}
			else {
			    log() << "**** Not unlocking for thread " << threadId << endl;
			    DistributedLock::killPinger( *myLock );
			    // We're simulating a crashed process...
			    break;
			}
		    }

                }
                catch( LockException& e ) {
                    log() << "*** !Could not try distributed lock." << m_caused_by(e) << endl;
                    break;
                }

                sleepmillis(rand_r(&seed) % threadSleep);
            }

            result << "errors" << errors
                   << "skew" << skew
                   << "takeover" << (long long) (legacy ? takeoverMS : takeoverMins)
                   << "localTimeout" << (takeoverMS > 0);

        }

        void test(ConnectionString& hostConn, string& lockName, unsigned seed) {
            return;
        }

        bool run(const string&, BSONObj& cmdObj, string& errmsg,
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
                errmsg = "Clocks could not be skewed." + m_caused_by(e);
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
            else{
            	log( LLL ) << "No host clocks to skew." << endl;
            	return;
            }

            log( LLL ) << "Skewing clocks of hosts " << cluster << endl;

            unsigned s = 0;
            for(vector<long long>::iterator i = skew.begin(); i != skew.end(); ++i,s++) {

                ConnectionString server( cluster.getServers()[s] );
                ScopedDbConnection conn( server );

                BSONObj result;
                try {
                    bool success = conn->runCommand( string("admin"), BSON( "_skewClockCommand" << 1 << "skew" << *i ), result );
                    // TODO:  Better error code
                    uassert_msg(70000, "Could not communicate with server " << server.toString() << " in cluster " << cluster.toString() << " to change skew by " << *i, success );

                    log( LLL + 1 ) << " Skewed host " << server << " clock by " << *i << endl;
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

        bool run(const string&, BSONObj& cmdObj, string& errmsg,
                 BSONObjBuilder& result, bool) {

            long long skew = (long long) number_field(cmdObj, "skew", 0);

            log() << "Adjusting jsTime() clock skew to " << skew << endl;

            jsTimeVirtualSkew( skew );

            log() << "JSTime adjusted, now is " << jsTime() << endl;

            return true;

        }

    } testSkewClockCommand;

}

