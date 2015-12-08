// distlock_test.cpp

/*    Copyright 2009 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/legacy/distlock.h"

#include <boost/thread/tss.hpp>
#include <iostream>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog/legacy/legacy_dist_lock_pinger.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/bson_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

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
#define string_field(obj, name, def) (obj.hasField(name) ? obj[name].String() : def)
#define number_field(obj, name, def) (obj.hasField(name) ? obj[name].Number() : def)

namespace mongo {

using std::shared_ptr;
using std::endl;
using std::string;
using std::stringstream;
using std::vector;

/**
 * Stress test distributed lock by running multiple threads to contend with a single lock.
 * Also has an option to make some thread terminate while holding the lock and have some
 * other thread take over it after takeoverMS has elapsed. Note that this test does not check
 * whether the lock was eventually overtaken and this is only valid if the LockPinger frequency
 * is faster than takeoverMS.
 *
 * Note: Running concurrent instances of this command is not recommended as it can result in
 * more frequent pings. And this effectively weakens the stress test.
 *
 * {
 *   _testDistLockWithSkew: 1,
 *
 *   lockName: <string for distributed lock>,
 *   host: <connection string for config server>,
 *   seed: <numeric seed for random generator>,
 *   numThreads: <num of threads to spawn and grab the lock>,
 *
 *   takeoverMS: <duration of missed ping in milliSeconds before a lock can be overtaken>,
 *   wait: <time in milliseconds before stopping the test threads>,
 *   skewHosts: <Array<Numeric>, numbers to be used when calling _skewClockCommand
 *              against each config server>,
 *   threadWait: <upper bound wait in milliSeconds while holding a lock>,
 *
 *   hangThreads: <integer n, where 1 out of n threads will abort after acquiring lock>,
 *   threadSleep: <upper bound sleep duration in mSecs between each round of lock operation>,
 *   skewRange: <maximum skew variance in milliSeconds for a thread's clock, delta will never
 *              be greater than skewRange/2>
 * }
 */
class TestDistLockWithSkew : public Command {
public:
    static const int logLvl = 1;

    TestDistLockWithSkew() : Command("_testDistLockWithSkew") {}
    virtual void help(stringstream& help) const {
        help << "should not be calling this directly" << endl;
    }

    virtual bool slaveOk() const {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }
    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}

    void runThread(ConnectionString& hostConn,
                   unsigned threadId,
                   unsigned seed,
                   LegacyDistLockPinger* pinger,
                   BSONObj& cmdObj,
                   BSONObjBuilder& result) {
        stringstream ss;
        ss << "thread-" << threadId;
        setThreadName(ss.str().c_str());

        // Lock name
        string lockName = string_field(cmdObj, "lockName", this->name + "_lock");

        // Range of clock skew in diff threads
        int skewRange = (int)number_field(cmdObj, "skewRange", 1);

        // How long to wait with the lock
        int threadWait = (int)number_field(cmdObj, "threadWait", 30);
        if (threadWait <= 0)
            threadWait = 1;

        // Max amount of time (ms) a thread waits before checking the lock again
        int threadSleep = (int)number_field(cmdObj, "threadSleep", 30);
        if (threadSleep <= 0)
            threadSleep = 1;

        // How long until the lock is forced in ms, only compared locally
        unsigned long long takeoverMS = (unsigned long long)number_field(cmdObj, "takeoverMS", 0);

        // Whether or not we should hang some threads
        int hangThreads = (int)number_field(cmdObj, "hangThreads", 0);


        boost::mt19937 gen((boost::mt19937::result_type)seed);

        boost::variate_generator<boost::mt19937&, boost::uniform_int<>> randomSkew(
            gen, boost::uniform_int<>(0, skewRange));
        boost::variate_generator<boost::mt19937&, boost::uniform_int<>> randomWait(
            gen, boost::uniform_int<>(1, threadWait));
        boost::variate_generator<boost::mt19937&, boost::uniform_int<>> randomSleep(
            gen, boost::uniform_int<>(1, threadSleep));
        boost::variate_generator<boost::mt19937&, boost::uniform_int<>> randomNewLock(
            gen, boost::uniform_int<>(0, 3));


        int skew = 0;
        if (!lock.get()) {
            // Pick a skew, but the first two threads skew the whole range
            if (threadId == 0)
                skew = -skewRange / 2;
            else if (threadId == 1)
                skew = skewRange / 2;
            else
                skew = randomSkew() - (skewRange / 2);

            // Skew this thread
            jsTimeVirtualThreadSkew(skew);

            log() << "Initializing lock with skew of " << skew << " for thread " << threadId
                  << endl;

            lock.reset(new DistributedLock(hostConn, lockName, takeoverMS, true));

            log() << "Skewed time " << jsTime() << "  for thread " << threadId << endl
                  << "  max wait (with lock: " << threadWait << ", after lock: " << threadSleep
                  << ")" << endl
                  << "  takeover in " << takeoverMS << "(ms remote)" << endl;
        }

        DistributedLock* myLock = lock.get();

        bool errors = false;
        BSONObj lockObj;
        while (keepGoing.loadRelaxed()) {
            Status pingStatus = pinger->startPing(
                *myLock, stdx::chrono::milliseconds(takeoverMS / LOCK_SKEW_FACTOR));

            if (!pingStatus.isOK()) {
                log() << "**** Not good for pinging: " << pingStatus;
                break;
            }

            try {
                if (myLock->lock_try(OID::gen(), "Testing distributed lock with skew.", &lockObj)) {
                    log() << "**** Locked for thread " << threadId << " with ts " << lockObj["ts"]
                          << endl;

                    if (count.loadRelaxed() % 3 == 1 &&
                        myLock->lock_try(OID::gen(), "Testing lock non-re-entry.")) {
                        errors = true;
                        log() << "**** !Invalid lock re-entry" << endl;
                        break;
                    }

                    int before = count.addAndFetch(1);
                    int sleep = randomWait();
                    sleepmillis(sleep);
                    int after = count.loadRelaxed();

                    if (after != before) {
                        errors = true;
                        log() << "**** !Bad increment while sleeping with lock for: " << sleep
                              << "ms" << endl;
                        break;
                    }

                    // Unlock only half the time...
                    if (hangThreads == 0 || threadId % hangThreads != 0) {
                        log() << "**** Unlocking for thread " << threadId << " with ts "
                              << lockObj["ts"] << endl;
                        myLock->unlock(lockObj["ts"].OID());
                    } else {
                        log() << "**** Not unlocking for thread " << threadId << endl;
                        pinger->stopPing(myLock->getRemoteConnection(), myLock->getProcessId());
                        // We're simulating a crashed process...
                        break;
                    }
                }

            } catch (const DBException& ex) {
                log() << "*** !Could not try distributed lock." << causedBy(ex) << endl;
                break;
            }

            // Create a new lock 1/3 of the time
            if (randomNewLock() > 1) {
                lock.reset(new DistributedLock(hostConn, lockName, takeoverMS, true));
                myLock = lock.get();
            }

            sleepmillis(randomSleep());
        }

        result << "errors" << errors << "skew" << skew << "takeover" << (long long)takeoverMS
               << "localTimeout" << (takeoverMS > 0);
    }

    void test(ConnectionString& hostConn, string& lockName, unsigned seed) {
        return;
    }

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        Timer t;

        ConnectionString hostConn(cmdObj["host"].String(), ConnectionString::SYNC);

        unsigned seed = (unsigned)number_field(cmdObj, "seed", 0);
        int numThreads = (int)number_field(cmdObj, "numThreads", 4);
        int wait = (int)number_field(cmdObj, "wait", 10000);

        log() << "Starting " << this->name << " with -" << endl
              << "  seed: " << seed << endl
              << "  numThreads: " << numThreads << endl
              << "  total wait: " << wait << endl
              << endl;

        // Skew host clocks if needed
        try {
            skewClocks(hostConn, cmdObj);
        } catch (DBException e) {
            errmsg = str::stream() << "Clocks could not be skewed." << causedBy(e);
            return false;
        }

        LegacyDistLockPinger pinger;

        count.store(0);
        keepGoing.store(true);

        vector<shared_ptr<stdx::thread>> threads;
        vector<shared_ptr<BSONObjBuilder>> results;
        for (int i = 0; i < numThreads; i++) {
            results.push_back(shared_ptr<BSONObjBuilder>(new BSONObjBuilder()));
            threads.push_back(shared_ptr<stdx::thread>(
                new stdx::thread(stdx::bind(&TestDistLockWithSkew::runThread,
                                            this,
                                            hostConn,
                                            (unsigned)i,
                                            seed + i,
                                            &pinger,
                                            stdx::ref(cmdObj),
                                            stdx::ref(*(results[i].get()))))));
        }

        sleepsecs(wait / 1000);
        keepGoing.store(false);

        bool errors = false;
        for (unsigned i = 0; i < threads.size(); i++) {
            threads[i]->join();
            errors = errors || results[i].get()->obj()["errors"].Bool();
        }

        result.append("count", count.loadRelaxed());
        result.append("errors", errors);
        result.append("timeMS", t.millis());

        pinger.shutdown(true);

        return !errors;
    }

    /**
     * Skews the clocks of a remote cluster by a particular amount, specified by
     * the "skewHosts" element in a BSONObj.
     */
    static void skewClocks(ConnectionString& cluster, BSONObj& cmdObj) {
        vector<long long> skew;
        if (cmdObj.hasField("skewHosts")) {
            bsonArrToNumVector<long long>(cmdObj["skewHosts"], skew);
        } else {
            LOG(logLvl) << "No host clocks to skew." << endl;
            return;
        }

        LOG(logLvl) << "Skewing clocks of hosts " << cluster << endl;

        unsigned s = 0;
        for (vector<long long>::iterator i = skew.begin(); i != skew.end(); ++i, s++) {
            ConnectionString server(cluster.getServers()[s]);
            ScopedDbConnection conn(server.toString());

            BSONObj result;
            try {
                bool success = conn->runCommand(
                    string("admin"), BSON("_skewClockCommand" << 1 << "skew" << *i), result);

                uassert(13678,
                        str::stream() << "Could not communicate with server " << server.toString()
                                      << " in cluster " << cluster.toString()
                                      << " to change skew by " << *i,
                        success);

                LOG(logLvl + 1) << " Skewed host " << server << " clock by " << *i << endl;
            } catch (...) {
                conn.done();
                throw;
            }

            conn.done();
        }
    }

    // variables for test
    boost::thread_specific_ptr<DistributedLock> lock;
    AtomicUInt32 count;
    AtomicWord<bool> keepGoing;
};

MONGO_INITIALIZER(RegisterDistLockWithSkewCmd)(InitializerContext* context) {
    if (Command::testCommandsEnabled) {
        // Leaked intentionally: a Command registers itself when constructed.
        new TestDistLockWithSkew();
    }
    return Status::OK();
}

/**
 * Utility command to virtually skew the clock of a mongo server a particular amount.
 * This skews the clock globally, per-thread skew is also possible.
 */
class SkewClockCommand : public Command {
public:
    SkewClockCommand() : Command("_skewClockCommand") {}
    virtual void help(stringstream& help) const {
        help << "should not be calling this directly" << endl;
    }

    virtual bool slaveOk() const {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }
    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        long long skew = (long long)number_field(cmdObj, "skew", 0);

        log() << "Adjusting jsTime() clock skew to " << skew << endl;

        jsTimeVirtualSkew(skew);

        log() << "JSTime adjusted, now is " << jsTime() << endl;

        return true;
    }
};
MONGO_INITIALIZER(RegisterSkewClockCmd)(InitializerContext* context) {
    if (Command::testCommandsEnabled) {
        // Leaked intentionally: a Command registers itself when constructed.
        new SkewClockCommand();
    }
    return Status::OK();
}
}
