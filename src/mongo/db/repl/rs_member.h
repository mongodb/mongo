// @file rsmember.h
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
 */

/** replica set member */

#pragma once

#include "../../util/concurrency/value.h"

namespace mongo {


    /*
        RS_STARTUP    serving still starting up, or still trying to initiate the set
        RS_PRIMARY    this server thinks it is primary
        RS_SECONDARY  this server thinks it is a secondary (slave mode)
        RS_RECOVERING recovering/resyncing; after recovery usually auto-transitions to secondary
        RS_FATAL      something bad has occurred and server is not completely offline with regard to the replica set.  fatal error.
        RS_STARTUP2   loaded config, still determining who is primary
    */
    struct MemberState {
        enum MS {
            RS_STARTUP = 0,
            RS_PRIMARY = 1,
            RS_SECONDARY = 2,
            RS_RECOVERING = 3,
            RS_FATAL = 4,
            RS_STARTUP2 = 5,
            RS_UNKNOWN = 6, /* remote node not yet reached */
            RS_ARBITER = 7,
            RS_DOWN = 8, /* node not reachable for a report */
            RS_ROLLBACK = 9,
            RS_SHUNNED = 10, /* node shunned from replica set */
        } s;

        MemberState(MS ms = RS_UNKNOWN) : s(ms) { }
        explicit MemberState(int ms) : s((MS) ms) { }

        bool startup() const { return s == RS_STARTUP; }
        bool primary() const { return s == RS_PRIMARY; }
        bool secondary() const { return s == RS_SECONDARY; }
        bool recovering() const { return s == RS_RECOVERING; }
        bool startup2() const { return s == RS_STARTUP2; }
        bool fatal() const { return s == RS_FATAL; }
        bool rollback() const { return s == RS_ROLLBACK; }
        bool readable() const { return s == RS_PRIMARY || s == RS_SECONDARY; }
        bool shunned() const { return s == RS_SHUNNED; }

        string toString() const;

        bool operator==(const MemberState& r) const { return s == r.s; }
        bool operator!=(const MemberState& r) const { return s != r.s; }
    };

    /* this is supposed to be just basic information on a member,
       and copy constructable. */
    class HeartbeatInfo {
        unsigned _id;
    public:
        HeartbeatInfo() : _id(0xffffffff), hbstate(MemberState::RS_UNKNOWN), health(-1.0),
            downSince(0), skew(INT_MIN), authIssue(false), ping(0) { }
        HeartbeatInfo(unsigned id);
        unsigned id() const { return _id; }
        MemberState hbstate;
        double health;
        time_t upSince;
        long long downSince;
        time_t lastHeartbeat;
        DiagStr lastHeartbeatMsg;
        OpTime opTime;
        int skew;
        bool authIssue;
        unsigned int ping; // milliseconds
        static unsigned int numPings;

        bool up() const { return health > 0; }

        /** health is set to -1 on startup.  that means we haven't even checked yet.  0 means we checked and it failed. */
        bool maybeUp() const { return health != 0; }

        long long timeDown() const; // ms

        /* true if changed in a way of interest to the repl set manager. */
        bool changed(const HeartbeatInfo& old) const;
    };

    inline HeartbeatInfo::HeartbeatInfo(unsigned id) : 
        _id(id), 
        authIssue(false),
        ping(0) {
        hbstate = MemberState::RS_UNKNOWN;
        health = -1.0;
        downSince = 0;
        lastHeartbeat = upSince = 0;
        skew = INT_MIN;
    }

    inline bool HeartbeatInfo::changed(const HeartbeatInfo& old) const {
        return health != old.health ||
               hbstate != old.hbstate;
    }

    inline string MemberState::toString() const {
        switch ( s ) {
        case RS_STARTUP: return "STARTUP";
        case RS_PRIMARY: return "PRIMARY";
        case RS_SECONDARY: return "SECONDARY";
        case RS_RECOVERING: return "RECOVERING";
        case RS_FATAL: return "FATAL";
        case RS_STARTUP2: return "STARTUP2";
        case RS_ARBITER: return "ARBITER";
        case RS_DOWN: return "DOWN";
        case RS_ROLLBACK: return "ROLLBACK";
        case RS_UNKNOWN: return "UNKNOWN";
        case RS_SHUNNED: return "REMOVED";
        }
        return "";
    }

}
