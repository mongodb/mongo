/*    Copyright 2013 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/server_options.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/synchronization.h"
#include "mongo/util/time_support.h"

namespace {

    using mongo::BackgroundJob;
    using mongo::MsgAssertionException;
    using mongo::stdx::mutex;
    using mongo::Notification;

    namespace stdx = mongo::stdx;

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
            Job() : _hasRun(false) {}

            virtual std::string name() const {
                return "BackgroundLifeCycle::CannotCallGoAgain";
            }

            virtual void run() {
                {
                    stdx::lock_guard<stdx::mutex> lock( _mutex );
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
