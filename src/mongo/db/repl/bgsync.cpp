/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/pch.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/client.h"

namespace mongo {

    bool ReplSetImpl::_isStale(OplogReader& r, const OpTime& startTs, BSONObj& remoteOldestOp) {
        remoteOldestOp = r.findOne(rsoplog, Query());
        OpTime remoteTs = remoteOldestOp["ts"]._opTime();
        DEV log() << "replSet remoteOldestOp:    " << remoteTs.toStringLong() << rsLog;
        else LOG(3) << "replSet remoteOldestOp: " << remoteTs.toStringLong() << rsLog;
        DEV {
            log() << "replSet lastOpTimeWritten: " << lastOpTimeWritten.toStringLong() << rsLog;
            log() << "replSet our state: " << state().toString() << rsLog;
        }
        if( startTs >= remoteTs ) {
            return false;
        }

        return true;
    }

    Member* ReplSetImpl::_getOplogReader(OplogReader& r, const OpTime& minTS) {
        Member *target = 0, *stale = 0;
        BSONObj oldest;

        verify(r.conn() == 0);

        while ((target = getMemberToSyncTo()) != 0) {
            string current = target->fullName();

            if( !r.connect(current) ) {
                log(2) << "replSet can't connect to " << current << " to read operations" << rsLog;
                r.resetConnection();
                veto(current);
                continue;
            }

            if( !minTS.isNull() && _isStale(r, minTS, oldest) ) {
                r.resetConnection();
                veto(current, 600);
                stale = target;
                continue;
            }

            // if we made it here, the target is up and not stale
            return target;
        }

        // the only viable sync target was stale
        if (stale) {
            log() << "replSet error RS102 too stale to catch up, at least from " << stale->fullName() << rsLog;
            log() << "replSet our last optime : " << lastOpTimeWritten.toStringLong() << rsLog;
            log() << "replSet oldest at " << stale->fullName() << " : " << oldest["ts"]._opTime().toStringLong() << rsLog;
            log() << "replSet See http://www.mongodb.org/display/DOCS/Resyncing+a+Very+Stale+Replica+Set+Member" << rsLog;

            // reset minvalid so that we can't become primary prematurely
            {
                Lock::DBWrite lk("local.replset.minvalid");
                Helpers::putSingleton("local.replset.minvalid", oldest);
            }

            sethbmsg("error RS102 too stale to catch up");
            changeState(MemberState::RS_RECOVERING);
            sleepsecs(120);
        }

        return 0;
    }

} // namespace mongo
