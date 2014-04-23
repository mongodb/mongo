/**
*    Copyright (C) 2008 10gen Inc.
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

#include <ctime>
#include <list>

#include "mongo/util/concurrency/mutex.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/optime.h"
#include "mongo/bson/bsonobj.h"

namespace mongo {

    class ReplSetImpl;
    struct Target;

    class Consensus {
    private:
        ReplSetImpl &rs;
        struct LastYea {
            LastYea() : when(0), who(0xffffffff) { }
            time_t when;
            unsigned who;
        };
        static SimpleMutex lyMutex;
        LastYea _ly;
        unsigned _yea(unsigned memberId); // throws VoteException
        void _electionFailed(unsigned meid);
        void _electSelf();
        bool _weAreFreshest(bool& allUp, int& nTies);
        bool _sleptLast; // slept last elect() pass

        // This is a unique id that is changed each time we transition to PRIMARY, as the
        // result of an election.
        OID _electionId;
        // PRIMARY server's time when the election to primary occurred
        OpTime _electionTime;

        int _totalVotes() const;
        void _multiCommand(BSONObj cmd, std::list<Target>& L);
    public:
        Consensus(ReplSetImpl *t) : rs(*t) {
            _sleptLast = false;
            steppedDown = 0;
        }

        /* if we've stepped down, this is when we are allowed to try to elect ourself again.
           todo: handle possible weirdnesses at clock skews etc.
        */
        time_t steppedDown;

        bool aMajoritySeemsToBeUp() const;
        bool shouldRelinquish() const;
        void electSelf();
        void electCmdReceived(BSONObj, BSONObjBuilder*);

        OID getElectionId() const { return _electionId; }
        void setElectionId(OID oid) { _electionId = oid; }
        OpTime getElectionTime() const { return _electionTime; }
        void setElectionTime(OpTime electionTime) { _electionTime = electionTime; }
    };
} // namespace mongo
