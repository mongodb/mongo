/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/base/init.h"
#include "mongo/db/curop.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    CmdLine cmdLine; // needed to satisfy reference in curop.h (and elsewhere)

    namespace {

        const long long intervalLong = 2000 * 1000; // 2s in micros
        const long long intervalShort = 10 * 1000; // 10ms in micros

        //
        // Before executing the TimeHasExpired suite, spawn a dummy listener thread to be the
        // process time tracker (the tests rely on Listener::_timeTracker being available).
        //

        class TestListener : public Listener {
        public:
            TestListener() : Listener("test", "", 0) {} // port 0 => any available high port
            virtual void acceptedMP(MessagingPort *mp) {}
        };

        void timeTrackerSetup() {
            TestListener listener;
            listener.setAsTimeTracker();
            listener.initAndListen();
        }

        MONGO_INITIALIZER(CurOpTest)(InitializerContext* context) {
            boost::thread t(timeTrackerSetup);

            // Wait for listener thread to start tracking time.
            while (Listener::getElapsedTimeMillis() == 0) {
                sleepmillis(10);
            }

            return Status::OK();
        }

        // Long operation + short timeout => time should expire.
        TEST(TimeHasExpired, PosSimple) {
            CurOp curOp(NULL);
            curOp.setMaxTimeMicros(intervalShort);
            curOp.ensureStarted();
            sleepmicros(intervalLong);
            ASSERT_TRUE(curOp.maxTimeHasExpired());
        }

        // Short operation + long timeout => time should not expire.
        TEST(TimeHasExpired, NegSimple) {
            CurOp curOp(NULL);
            curOp.setMaxTimeMicros(intervalLong);
            curOp.ensureStarted();
            sleepmicros(intervalShort);
            ASSERT_FALSE(curOp.maxTimeHasExpired());
        }

    } // namespace

} // namespace mongo
