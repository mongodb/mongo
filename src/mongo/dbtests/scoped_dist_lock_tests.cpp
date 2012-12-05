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

#include <boost/thread/thread.hpp>

#include "mongo/client/distlock.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/client.h"
#include "mongo/db/instance.h"
#include "mongo/s/cluster_constants.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    /**
     * Fixture for testing config database upgrade
     */
    class ScopedDistLockTests: public mongo::unittest::Test, ConnectionString::ConnectionHook {
    public:

        class CustomDirectClient: public DBDirectClient {
        public:
            virtual ConnectionString::ConnectionType type() const {
                return ConnectionString::CUSTOM;
            }
        };

        virtual DBClientBase* connect(const ConnectionString& connStr,
                                      string& errmsg,
                                      double socketTimeout)
        {
            // Note - must be new, since it gets owned elsewhere
            return new CustomDirectClient();
        }

        DBClientBase& client() {
            return _client;
        }

        void lockForSecs(const ConnectionString& configLoc, const string& lockName, int lockSecs) {

            DistributedLock* distLock = new DistributedLock(configLoc, lockName);

            BSONObj otherLock;
            ASSERT(distLock->lock_try("just because", false, &otherLock));

            boost::thread(waitAndUnlock, distLock, lockSecs);
        }

        static void waitAndUnlock(DistributedLock* distLock, int waitSecs) {
            // Needed otherwise unlock throws an error
            Client::initThread("unlock thread");

            sleepsecs(waitSecs);
            distLock->unlock();
            delete distLock;
        }

    protected:

        void setUp() {
            DBException::traceExceptions = true;
            // Make all connections redirect to the direct client
            ConnectionString::setConnectionHook(this);
            // Disable the lock pinger
            setLockPingerEnabled(false);

            // Create the default config database before querying, necessary for direct connections
            client().dropDatabase("config");
            client().insert("config.test", BSON( "hello" << "world" ));
            client().dropCollection("config.test");
        }

        void tearDown() {
            DBException::traceExceptions = false;
            // Make all connections redirect to the direct client
            ConnectionString::setConnectionHook(NULL);
            // Reset the pinger
            setLockPingerEnabled(true);
        }

        CustomDirectClient _client;
    };

    /**
     * Tests that the ScopedDistributedLock respects the DistributedLock and vice-versa.
     */
    TEST_F(ScopedDistLockTests, BasicLockTest) {

        ConnectionString configLoc(string("$dummy:10000"));
        string lockName = "fooBar";

        //
        // Make sure the ScopedDistributedLock is blocked by a distributed lock
        //

        DistributedLock blockingLock(configLoc, lockName);

        BSONObj otherLock;
        ASSERT(blockingLock.lock_try("just because", false, &otherLock));

        {
            ScopedDistributedLock scopedLock(configLoc, lockName, "another reason");

            Timer timer;
            string errMsg;

            ASSERT(!scopedLock.tryAcquire(3 * 1000, &errMsg));
            ASSERT(errMsg != "");
            ASSERT(timer.seconds() >= 3);
        }

        blockingLock.unlock();

        //
        // Now try the reverse, and ensure the scoped lock is unlocked after it falls out of scope
        //

        {
            ScopedDistributedLock scopedLock(configLoc, lockName, "another reason");

            string errMsg;
            ASSERT(scopedLock.tryAcquire(0, &errMsg));
            ASSERT(errMsg == "");

            // Make sure acquiring again doesn't cause problems, should just pass-through
            ASSERT(scopedLock.tryAcquire(0, &errMsg));
            ASSERT(errMsg == "");

            ASSERT(!blockingLock.lock_try("just because", false, &otherLock));
        }

        ASSERT(blockingLock.lock_try("just because", false, &otherLock));
        blockingLock.unlock();
    }

    /**
     * Tests that the ScopedDistributedLock respects the DistributedLock and vice-versa.
     */
    TEST_F(ScopedDistLockTests, WaitForLockTest) {

        ConnectionString configLoc(string("$dummy:10000"));
        string lockName = "fooBar";

        //
        // Make sure we can wait for no time and a finite amount of time
        //

        log() << "Waiting for timed lock for 15 mins..." << endl;

        {
            ScopedDistributedLock scopedLock(configLoc, lockName, "another reason");

            lockForSecs(configLoc, lockName, 3);

            string errMsg;
            ASSERT(!scopedLock.tryAcquire(0, &errMsg));
            ASSERT(errMsg != "");

            ASSERT(scopedLock.tryAcquire(15 * 60 * 1000, &errMsg));
            ASSERT(errMsg == "");
        }

        //
        // Make sure we can wait for an undefined amount of time
        //

        log() << "Waiting for timed lock for undefined time..." << endl;

        {
            ScopedDistributedLock scopedLock(configLoc, lockName, "another reason");

            lockForSecs(configLoc, lockName, 3);

            string errMsg;
            ASSERT(scopedLock.tryAcquire(-1, &errMsg));
            ASSERT(errMsg == "");
        }

        // Quickly re-lock to make sure the scoped lock actually released.
        DistributedLock blockingLock(configLoc, lockName);
        BSONObj otherLock;
        ASSERT(blockingLock.lock_try("just because", false, &otherLock));
        blockingLock.unlock();
    }

} // end namespace

