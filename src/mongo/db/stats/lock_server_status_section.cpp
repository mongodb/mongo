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

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/lock_stats.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"

namespace mongo {
namespace {

    class GlobalLockServerStatusSection : public ServerStatusSection {
    public:
        GlobalLockServerStatusSection() : ServerStatusSection("globalLock") {
            _started = curTimeMillis64();
        }

        virtual bool includeByDefault() const { return true; }

        virtual BSONObj generateSection(OperationContext* txn,
                                        const BSONElement& configElement) const {

            int numTotal = 0;
            int numWriteLocked = 0;
            int numReadLocked = 0;
            int numWaitingRead = 0;
            int numWaitingWrite = 0;

            // This returns the blocked lock states
            {
                boost::lock_guard<boost::mutex> scopedLock(Client::clientsMutex);

                // Count all clients
                numTotal = Client::clients.size();

                ClientSet::const_iterator it = Client::clients.begin();
                for (; it != Client::clients.end(); it++) {
                    Client* client = *it;
                    invariant(client);

                    boost::unique_lock<Client> uniqueLock(*client);

                    const OperationContext* opCtx = client->getOperationContext();
                    if (opCtx == NULL) continue;

                    if (opCtx->lockState()->isWriteLocked()) {
                        numWriteLocked++;

                        if (opCtx->lockState()->getWaitingResource().isValid()) {
                            numWaitingWrite++;
                        }
                    }
                    else if (opCtx->lockState()->isReadLocked()) {
                        numReadLocked++;

                        if (opCtx->lockState()->getWaitingResource().isValid()) {
                            numWaitingRead++;
                        }
                    }
                }
            }

            // Construct the actual return value out of the mutex
            BSONObjBuilder ret;

            ret.append("totalTime", (long long)(1000 * (curTimeMillis64() - _started)));

            {
                BSONObjBuilder currentQueueBuilder(ret.subobjStart("currentQueue"));

                currentQueueBuilder.append("total", numWaitingRead + numWaitingWrite);
                currentQueueBuilder.append("readers", numWaitingRead);
                currentQueueBuilder.append("writers", numWaitingWrite);
                currentQueueBuilder.done();
            }

            {
                BSONObjBuilder activeClientsBuilder(ret.subobjStart("activeClients"));

                activeClientsBuilder.append("total", numTotal);
                activeClientsBuilder.append("readers", numReadLocked);
                activeClientsBuilder.append("writers", numWriteLocked);
                activeClientsBuilder.done();
            }

            ret.done();

            return ret.obj();
        }

    private:
        unsigned long long _started;

    } globalLockServerStatusSection;


    class LockStatsServerStatusSection : public ServerStatusSection {
    public:
        LockStatsServerStatusSection() : ServerStatusSection("locks") { }

        virtual bool includeByDefault() const { return true; }

        virtual BSONObj generateSection(OperationContext* txn,
                                        const BSONElement& configElement) const {
            BSONObjBuilder ret;

            SingleThreadedLockStats stats;
            reportGlobalLockingStats(&stats);

            stats.report(&ret);

            return ret.obj();
        }

    } lockStatsServerStatusSection;

} // namespace
} // namespace mongo
