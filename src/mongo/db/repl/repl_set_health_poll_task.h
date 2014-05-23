/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include <string>

#include "mongo/db/repl/heartbeat_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

    /**
     * Poll every other set member to check its status.
     *
     * A detail about local machines and authentication: suppose we have 2
     * members, A and B, on the same machine using different keyFiles. A is
     * primary. If we're just starting the set, there are no admin users, so A
     * and B can access each other because it's local access.
     *
     * Then we add a user to A. B cannot sync this user from A, because as soon
     * as we add a an admin user, A requires auth. However, A can still
     * heartbeat B, because B *doesn't* have an admin user.  So A can reach B
     * but B cannot reach A.
     *
     * Once B is restarted with the correct keyFile, everything should work as
     * expected.
     */
    class ReplSetHealthPollTask : public task::Task {
    private:
        /**
         * Each healthpoll task reconnects periodically.  By starting each task at a different
         * number for "tries", the tasks will reconnect at different times, minimizing the impact
         * of network blips.
         */
        static int s_try_offset;

        HostAndPort h;
        HeartbeatInfo m;
        int tries;
        const int threshold;
    public:
        ReplSetHealthPollTask(const HostAndPort& hh, const HeartbeatInfo& mm);

        string name() const { return "rsHealthPoll"; }

        void setUp() { }

        void doWork();

    private:
        bool tryHeartbeat(BSONObj* info, int* theirConfigVersion);

        bool _requestHeartbeat(HeartbeatInfo& mem, BSONObj& info, int& theirConfigVersion);

        void authIssue(HeartbeatInfo& mem);

        void down(HeartbeatInfo& mem, string msg);

        void up(const BSONObj& info, HeartbeatInfo& mem);

        // Heartbeat timeout
        time_t _timeout;
    };

} // namespace repl
} // namespace mongo
