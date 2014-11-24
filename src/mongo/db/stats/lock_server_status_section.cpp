/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/server_status.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/operation_context.h"


namespace mongo {

    /**
     * This is passed to the iterator for global environments and aggregates information about the
     * locks which are currently being held or waited on.
     */
    class LockStateAggregator : public GlobalEnvironmentExperiment::ProcessOperationContext {
    public:
        LockStateAggregator(bool blockedOnly) 
            : numWriteLocked(0),
              numReadLocked(0),
              _blockedOnly(blockedOnly) {

        }

        virtual void processOpContext(OperationContext* txn) {
            if (_blockedOnly && !txn->lockState()->hasLockPending()) {
                return;
            }

            if (txn->lockState()->isWriteLocked()) {
                numWriteLocked++;
            }
            else {
                numReadLocked++;
            }
        }

        int numWriteLocked;
        int numReadLocked;

    private:
        const bool _blockedOnly;
    };


    class GlobalLockServerStatusSection : public ServerStatusSection {
    public:
        GlobalLockServerStatusSection() : ServerStatusSection("globalLock"){
            _started = curTimeMillis64();
        }

        virtual bool includeByDefault() const { return true; }

        virtual BSONObj generateSection(OperationContext* txn,
                                        const BSONElement& configElement) const {

            BSONObjBuilder t;

            t.append("totalTime", (long long)(1000 * (curTimeMillis64() - _started)));

            // SERVER-14978: Need to report the global lock statistics somehow
            //
            // t.append( "lockTime" , qlk.stats.getTimeLocked( 'W' ) );

            // This returns the blocked lock states
            {
                BSONObjBuilder ttt(t.subobjStart("currentQueue"));

                LockStateAggregator blocked(true);
                getGlobalEnvironment()->forEachOperationContext(&blocked);

                ttt.append("total", blocked.numReadLocked + blocked.numWriteLocked);
                ttt.append("readers", blocked.numReadLocked);
                ttt.append("writers", blocked.numWriteLocked);
                ttt.done();
            }

            // This returns all the active clients (including those holding locks)
            {
                BSONObjBuilder ttt(t.subobjStart("activeClients"));

                LockStateAggregator active(false);
                getGlobalEnvironment()->forEachOperationContext(&active);

                ttt.append("total", active.numReadLocked + active.numWriteLocked);
                ttt.append("readers", active.numReadLocked);
                ttt.append("writers", active.numWriteLocked);
                ttt.done();
            }

            return t.obj();
        }

    private:
        unsigned long long _started;

    } globalLockServerStatusSection;


    class LockStatsServerStatusSection : public ServerStatusSection {
    public:
        LockStatsServerStatusSection() : ServerStatusSection("locks"){}
        virtual bool includeByDefault() const { return true; }

        BSONObj generateSection(OperationContext* txn,
                                const BSONElement& configElement) const {

            BSONObjBuilder b;

            // SERVER-14978: Need to report the global and per-DB lock stats here
            //
            // b.append(".", qlk.stats.report());

            return b.obj();
        }

    } lockStatsServerStatusSection;

} // namespace mongo
