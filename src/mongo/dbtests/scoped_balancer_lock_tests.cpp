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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/client.h"
#include "mongo/db/instance.h"
#include "mongo/s/balancer_lock.h"
#include "mongo/s/cluster_constants.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    /**
     * Fixture for testing config database upgrade
     */
    class ScopedBalancerLockTests: public mongo::unittest::Test, ConnectionString::ConnectionHook {
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

        void setBalancerStopped(bool stopped) {
            client().remove(ConfigNS::settings, BSONObj());
            client().insert(ConfigNS::settings,
                            BSON("_id" << "balancer" << SettingsFields::balancerStopped(stopped)));
        }

        bool isBalancerStopped() {
            BSONObj balancerDoc = client().findOne(ConfigNS::settings, BSON("_id" << "balancer"));
            if (balancerDoc.isEmpty()) return false;
            return balancerDoc[SettingsFields::balancerStopped()].trueValue();
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
    TEST_F(ScopedBalancerLockTests, BasicLockTest) {

        ConnectionString configLoc(string("$dummy:10000"));

        // Balancer by default is not stopped

        //
        // Now try the reverse, and ensure the scoped lock is unlocked after it falls out of scope
        //

        {
            ScopedBalancerLock scopedLock(configLoc, "some reason");

            string errMsg;
            ASSERT(scopedLock.tryAcquire(0, &errMsg));
            ASSERT(errMsg == "");
        }

        ASSERT(!isBalancerStopped());

        setBalancerStopped(false);

        {
            ScopedBalancerLock scopedLock(configLoc, "some reason");

            string errMsg;
            ASSERT(scopedLock.tryAcquire(0, &errMsg));
            ASSERT(errMsg == "");
        }

        ASSERT(!isBalancerStopped());

        setBalancerStopped(true);

        {
            ScopedBalancerLock scopedLock(configLoc, "some reason");

            string errMsg;
            ASSERT(scopedLock.tryAcquire(0, &errMsg));
            ASSERT(errMsg == "");
        }

        ASSERT(isBalancerStopped());
    }

} // end namespace

