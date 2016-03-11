/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/exit.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {

const long long kSleepMillis = 5000;
const long long kEpsilon = 3000;

TEST(Listener, ElapsedTimeCheck) {
    Listener listener("test_listener", "", 0);  // port 0 => any available high port
    listener.setupSockets();
    stdx::thread t([&listener]() { listener.initAndListen(); });
    listener.waitUntilListening();
    sleepmillis(kSleepMillis);  // wait for setup.
    long long listenStart = listener.getMyElapsedTimeMillis();
    long long clockStart = curTimeMillis64();
    sleepmillis(kSleepMillis);
    long long listenDelta = listener.getMyElapsedTimeMillis() - listenStart;
    long long clockDelta = curTimeMillis64() - clockStart;
    log() << "Listener elapsed time: " << listenDelta << std::endl;
    log() << "Clock elapsed time:    " << clockDelta << std::endl;
    ASSERT_APPROX_EQUAL(listenDelta, clockDelta, kEpsilon);
    shutdownNoTerminate();
    t.join();
}

}  // namespace

}  // namespace mongo
