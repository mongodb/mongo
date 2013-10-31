/*    Copyright 2013 10gen Inc.
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

#include "mongo/platform/basic.h"

#include <boost/thread/thread.hpp>

#include "mongo/db/server_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/synchronization.h"
#include "mongo/util/time_support.h"

namespace {

    using mongo::BackgroundJob;
    using mongo::MsgAssertionException;
    using mongo::mutex;
    using mongo::Notification;

    // a global variable that can be accessed independent of the IncTester object below
    // IncTester keeps it up-to-date
    int GLOBAL_val;

    class IncTester : public mongo::BackgroundJob {
    public:
        explicit IncTester( long long millis , bool selfDelete = false )
            : mongo::BackgroundJob(selfDelete), _val(0), _millis(millis) { GLOBAL_val = 0; }

        void waitAndInc( long long millis ) {
            if ( millis )
                mongo::sleepmillis( millis );
            ++_val;
            ++GLOBAL_val;
        }

        int getVal() { return _val; }

        /* --- BackgroundJob virtuals --- */

        std::string name() const { return "IncTester"; }

        void run() { waitAndInc( _millis ); }

    private:
        int _val;
        long long _millis;
    };

    TEST(BackgroundJobBasic, NormalCase) {
        IncTester tester( 0 /* inc without wait */ );
        tester.go();
        ASSERT( tester.wait() );
        ASSERT_EQUALS( tester.getVal() , 1 );
    }

    TEST(BackgroundJobBasic, TimeOutCase) {
        IncTester tester( 2000 /* wait 2 sec before inc-ing */ );
        tester.go();
        ASSERT( ! tester.wait( 100 /* ms */ ) ); // should time out
        ASSERT_EQUALS( tester.getVal() , 0 );

        // if we wait longer than the IncTester, we should see the increment
        ASSERT( tester.wait( 4000 /* ms */ ) );  // should not time out
        ASSERT_EQUALS( tester.getVal() , 1 );
    }

    TEST(BackgroundJobBasic, SelfDeletingCase) {
        mongo::BackgroundJob* j = new IncTester( 0 /* inc without wait */ , true /* self delete */);
        j->go();

        // the background thread should have continued running and this test should pass the
        // heap-checker as well
        mongo::sleepmillis( 1000 );
        ASSERT_EQUALS( GLOBAL_val, 1 );
    }

    TEST(BackgroundJobLifeCycle, Go) {

        class Job : public BackgroundJob {
        public:
            Job()
                : _mutex("BackgroundJobLifeCycle::Go")
                , _hasRun(false) {}

            virtual std::string name() const {
                return "BackgroundLifeCycle::CannotCallGoAgain";
            }

            virtual void run() {
                {
                    mongo::scoped_lock lock( _mutex );
                    ASSERT_FALSE( _hasRun );
                    _hasRun = true;
                }

                _n.waitToBeNotified();
            }

            void notify() {
                _n.notifyOne();
            }

        private:
            mutex _mutex;
            bool _hasRun;
            Notification _n;
        };

        Job j;

        // This call starts Job running.
        j.go();

        // Calling 'go' again while it is running is an error.
        ASSERT_THROWS(j.go(), MsgAssertionException);

        // Stop the Job
        j.notify();
        j.wait();

        // Calling 'go' on a done task is a no-op. If it were not,
        // we would fail the assert in Job::run above.
        j.go();
    }

} // namespace
