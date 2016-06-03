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

/** @file perftests.cpp.cpp : unit tests relating to performance

          The idea herein is tests that run fast and can be part of the normal CI suite.  So no
          tests herein that take a long time to run.  Obviously we need those too, but they will be
          separate.

          These tests use DBDirectClient; they are a bit white-boxish.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <boost/thread/condition.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <iomanip>
#include <iostream>
#include <mutex>

#include "mongo/config.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/client.h"
#include "mongo/db/db.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/storage/mmap_v1/dur_stats.h"
#include "mongo/db/storage/mmap_v1/mmap.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/dbtests/framework_options.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

namespace PerfTests {

using std::cout;
using std::endl;
using std::fixed;
using std::left;
using std::min;
using std::right;
using std::setprecision;
using std::setw;
using std::string;
using std::vector;

namespace dps = ::mongo::dotted_path_support;

const bool profiling = false;

class ClientBase {
public:
    ClientBase() : _client(&_txn) {
        mongo::LastError::get(_txn.getClient()).reset();
    }
    virtual ~ClientBase() {
        mongo::LastError::get(_txn.getClient()).reset();
    }

protected:
    void insert(const char* ns, BSONObj o) {
        _client.insert(ns, o);
    }
    void update(const char* ns, BSONObj q, BSONObj o, bool upsert = 0) {
        _client.update(ns, Query(q), o, upsert);
    }
    bool error() {
        return !_client.getPrevError().getField("err").isNull();
    }

    DBClientBase* client() {
        return &_client;
    }
    OperationContext* txn() {
        return &_txn;
    }

private:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;
    DBDirectClient _client;
};

static std::shared_ptr<DBClientConnection> conn;
static string _perfhostname;

class B : public ClientBase {
    string _ns;

protected:
    const char* ns() {
        return _ns.c_str();
    }

    // anything you want to do before being timed
    virtual void prep() {}

    // anything you want to do before threaded test
    virtual void prepThreaded() {}

    virtual void timed() = 0;

    // optional 2nd test phase to be timed separately. You must provide it with a unique
    // name in order for it to run by overloading 'name2'.
    virtual void timed2(DBClientBase*) {}

    // return name of second test.
    virtual string name2() {
        return name();
    }

    virtual void post() {}

    virtual string name() = 0;

    // how long to run test.  0 is a sentinel which means just run the timed() method once and time
    // it.
    virtual int howLongMillis() {
        return profiling ? 30000 : 5000;
    }

    /* override if your test output doesn't need that */
    virtual bool showDurStats() {
        return true;
    }

public:
    virtual unsigned batchSize() {
        return 50;
    }

    void say(unsigned long long n, long long us, string s) {
        unsigned long long rps = (n * 1000 * 1000) / (us > 0 ? us : 1);
        cout << "stats " << setw(42) << left << s << ' ' << right << setw(9) << rps << ' ' << right
             << setw(5) << us / 1000 << "ms ";
        if (showDurStats()) {
            cout << dur::stats.curr()->_asCSV();
        }
        cout << endl;

        if (conn && !conn->isFailed()) {
            const char* ns = "perf.pstats";
            if (frameworkGlobalParams.perfHist) {
                static bool needver = true;
                try {
                    // try to report rps from last time */
                    Query q;
                    {
                        BSONObjBuilder b;
                        b.append("host", _perfhostname);
                        b.append("test", s);
                        b.append("dur", storageGlobalParams.dur);
                        DEV {
                            b.append("info.DEBUG", true);
                        }
                        else b.appendNull("info.DEBUG");
                        if (sizeof(int*) == 4)
                            b.append("info.bits", 32);
                        else
                            b.appendNull("info.bits");
                        q = Query(b.obj()).sort("when", -1);
                    }
                    BSONObj fields = BSON("rps" << 1 << "info" << 1);
                    vector<BSONObj> v;
                    conn->findN(v, ns, q, frameworkGlobalParams.perfHist, 0, &fields);
                    for (vector<BSONObj>::iterator i = v.begin(); i != v.end(); i++) {
                        BSONObj o = *i;
                        double lastrps = o["rps"].Number();
                        if (0 && lastrps) {
                            cout << "stats " << setw(42) << right << "new/old:" << ' ' << setw(9);
                            cout << fixed << setprecision(2) << rps / lastrps;
                            if (needver) {
                                cout << "         "
                                     << dps::extractElementAtPath(o, "info.git").toString();
                            }
                            cout << '\n';
                        }
                    }
                } catch (...) {
                }
                cout.flush();
                needver = false;
            }
            {
                bob b;
                b.append("host", _perfhostname);
                b.appendTimeT("when", time(0));
                b.append("test", s);
                b.append("rps", (int)rps);
                b.append("millis", us / 1000);
                b.appendBool("dur", storageGlobalParams.dur);
                if (showDurStats() && storageGlobalParams.dur) {
                    b.append("durStats", dur::stats.asObj());
                }

                {
                    bob inf;
                    inf.append("version", versionString);
                    if (sizeof(int*) == 4)
                        inf.append("bits", 32);
                    DEV inf.append("DEBUG", true);
#if defined(_WIN32)
                    inf.append("os", "win");
#endif
                    inf.append("git", gitVersion());
#ifdef MONGO_CONFIG_SSL
                    inf.append("OpenSSL", openSSLVersion());
#endif
                    inf.append("boost", BOOST_VERSION);
                    b.append("info", inf.obj());
                }
                BSONObj o = b.obj();
                // cout << "inserting " << o.toString() << endl;
                try {
                    conn->insert(ns, o);
                } catch (std::exception& e) {
                    warning() << "couldn't save perf results: " << e.what() << endl;
                }
            }
        }
    }

    /** if true runs timed2() again with several threads (8 at time of this writing).
    */
    virtual bool testThreaded() {
        return false;
    }

    int howLong() {
        int hlm = howLongMillis();
        DEV {
            // don't run very long with in debug mode - not very meaningful anyway on that build
            hlm = min(hlm, 500);
        }
        return hlm;
    }

    void run() {
        unsigned long long n = 0;

        _ns = string("perftest.") + name();
        client()->dropCollection(ns());
        prep();
        int hlm = howLong();
        mongo::Timer t;
        n = 0;
        const unsigned int Batch = batchSize();

        if (hlm == 0) {
            // means just do once
            timed();
        } else {
            do {
                unsigned int i;
                for (i = 0; i < Batch; i++)
                    timed();
                n += i;
            } while (t.micros() < (hlm * 1000));
        }

        client()->getLastError();  // block until all ops are finished

        say(n, t.micros(), name());

        post();

        string test2name = name2();
        {
            if (test2name != name()) {
                dur::stats.curr()->reset();
                mongo::Timer t;
                unsigned long long n = 0;
                while (1) {
                    unsigned int i;
                    for (i = 0; i < Batch; i++)
                        timed2(client());
                    n += i;
                    if (t.millis() > hlm)
                        break;
                }
                say(n, t.micros(), test2name);
            }
        }

        if (testThreaded()) {
            const int nThreads = 8;
            // cout << "testThreaded nThreads:" << nThreads << endl;
            mongo::Timer t;
            const unsigned long long result = launchThreads(nThreads);
            say(result / nThreads, t.micros(), test2name + "-threaded");
        }
    }

    bool stop;

    void thread(unsigned long long* counter) {
#if defined(_WIN32)
        static int z;
        srand(++z ^ (unsigned)time(0));
#endif
        Client::initThreadIfNotAlready("perftestthr");
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        DBDirectClient c(&txn);

        const unsigned int Batch = batchSize();
        prepThreaded();
        while (1) {
            unsigned int i = 0;
            for (i = 0; i < Batch; i++)
                timed2(&c);
            *counter += i;
            if (stop)
                break;
        }
    }

    unsigned long long launchThreads(int remaining) {
        stop = false;
        if (!remaining) {
            int hlm = howLong();
            sleepmillis(hlm);
            stop = true;
            return 0;
        }
        unsigned long long counter = 0;
        stdx::thread athread(stdx::bind(&B::thread, this, &counter));
        unsigned long long child = launchThreads(remaining - 1);
        athread.join();
        unsigned long long accum = child + counter;
        return accum;
    }
};

SimpleMutex m;
boost::mutex mboost;              // NOLINT
boost::timed_mutex mboost_timed;  // NOLINT
std::mutex mstd;                  // NOLINT
std::timed_mutex mstd_timed;      // NOLINT
SpinLock s;
stdx::condition_variable c;

class boostmutexspeed : public B {
public:
    string name() {
        return "boost::mutex";
    }
    virtual int howLongMillis() {
        return 500;
    }
    virtual bool showDurStats() {
        return false;
    }
    void timed() {
        boost::lock_guard<boost::mutex> lk(mboost);  // NOLINT
    }
};
class boosttimed_mutexspeed : public B {
public:
    string name() {
        return "boost::timed_mutex";
    }
    virtual int howLongMillis() {
        return 500;
    }
    virtual bool showDurStats() {
        return false;
    }
    void timed() {
        boost::lock_guard<boost::timed_mutex> lk(mboost_timed);  // NOLINT
    }
};
class simplemutexspeed : public B {
public:
    string name() {
        return "simplemutex";
    }
    virtual int howLongMillis() {
        return 500;
    }
    virtual bool showDurStats() {
        return false;
    }
    void timed() {
        stdx::lock_guard<SimpleMutex> lk(m);
    }
};

class stdmutexspeed : public B {
public:
    string name() {
        return "std::mutex";
    }
    virtual int howLongMillis() {
        return 500;
    }
    virtual bool showDurStats() {
        return false;
    }
    void timed() {
        std::lock_guard<std::mutex> lk(mstd);  // NOLINT
    }
};
class stdtimed_mutexspeed : public B {
public:
    string name() {
        return "std::timed_mutex";
    }
    virtual int howLongMillis() {
        return 500;
    }
    virtual bool showDurStats() {
        return false;
    }
    void timed() {
        std::lock_guard<std::timed_mutex> lk(mstd_timed);  // NOLINT
    }
};


class All : public Suite {
public:
    All() : Suite("perf") {}

    void setupTests() {
        cout << "stats test                                       rps------  time-- "
             << dur::stats.curr()->_CSVHeader() << endl;
        add<simplemutexspeed>();
        add<boostmutexspeed>();
        add<boosttimed_mutexspeed>();
        add<stdmutexspeed>();
        add<stdtimed_mutexspeed>();
    }
} myall;
}
