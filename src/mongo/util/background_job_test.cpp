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

#include "mongo/db/server_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/synchronization.h"

namespace mongo {

    ServerGlobalParams serverGlobalParams;

    bool inShutdown() {
        return false;
    }

} // namespace mongo

namespace {

    using mongo::BackgroundJob;
    using mongo::MsgAssertionException;
    using mongo::mutex;
    using mongo::Notification;

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
