/*
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

#include <climits>

#include "mongo/bson/optime.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/util/concurrency/value.h"

namespace mongo {
namespace repl {

    /* this is supposed to be just basic information on a member,
       and copy constructable. */
    class HeartbeatInfo {
    public:
        HeartbeatInfo();
        HeartbeatInfo(int id);
        int id() const { return _id; }
        MemberState hbstate;
        double health;
        time_t upSince;
        long long downSince;
        // This is the last time we got a response from a heartbeat request to a given member.
        time_t lastHeartbeat;
        // This is the last time we got a heartbeat request from a given member.
        time_t lastHeartbeatRecv;
        DiagStr lastHeartbeatMsg;
        DiagStr syncingTo;
        OpTime opTime;
        int skew;
        bool authIssue;
        unsigned int ping; // milliseconds
        static unsigned int numPings;

        // Time node was elected primary
        OpTime electionTime;

        bool up() const { return health > 0; }

        /** health is set to -1 on startup.  that means we haven't even checked yet.  0 means we
         *  checked and it failed.
         */
        bool maybeUp() const { return health != 0; }

        /* true if changed in a way of interest to the repl set manager. */
        bool changed(const HeartbeatInfo& old) const;

        /**
         * Updates this with the info received from the command result we got from
         * the last replSetHeartbeat.
         */
        void updateFromLastPoll(const HeartbeatInfo& newInfo);
    private:
        int _id;
    };

} // namespace repl
} // namespace mongo
