/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/repl_set_dist_lock_manager.h"

#include "mongo/s/dist_lock_logic.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

    using std::string;
    using stdx::chrono::milliseconds;

    ReplSetDistLockManager::ReplSetDistLockManager(CatalogManager* lockCatalogue):
        _lockCatalogue(lockCatalogue) {
    }

    void ReplSetDistLockManager::startUp() {
        // TODO: start background task.
    }

    void ReplSetDistLockManager::shutDown() {
        // TODO
    }

    StatusWith<DistLockManager::ScopedDistLock> ReplSetDistLockManager::lock(
            StringData name,
            StringData whyMessage,
            milliseconds waitFor,
            milliseconds lockTryInterval) {
        auto lastStatus = StatusWith<DistLockHandle>(
                ErrorCodes::LockBusy, str::stream() << "timed out waiting for " << name);

        Timer timer;
        Timer msgTimer;
        while (waitFor <= milliseconds::zero() || timer.millis() < waitFor.count()) {
            lastStatus = dist_lock_logic::lock(_lockCatalogue,
                                               name.toString(),
                                               whyMessage.toString());

            if (lastStatus.isOK()) {
                // TODO: add to pinger.
                return StatusWith<ScopedDistLock>(ScopedDistLock(lastStatus.getValue(), this));
            }

            if (waitFor.count() == 0) break;

            if (lastStatus.getStatus() != ErrorCodes::LockBusy) {
                return StatusWith<ScopedDistLock>(lastStatus.getStatus());
            }

            // Periodically message for debugging reasons
            if (msgTimer.seconds() > 10) {
                LOG(0) << "waited " << timer.seconds() << "s for distributed lock " << name
                       << " for " << whyMessage << ": " << causedBy(lastStatus.getStatus());

                msgTimer.reset();
            }

            milliseconds timeRemaining =
                    std::max(milliseconds::zero(), waitFor - milliseconds(timer.millis()));
            sleepmillis(std::min(lockTryInterval, timeRemaining).count());
        }

        return StatusWith<ScopedDistLock>(lastStatus.getStatus());
    }

    void ReplSetDistLockManager::unlock(const DistLockHandle& lockHandle) BOOST_NOEXCEPT {
        // TODO: stop pinging.

        if (!dist_lock_logic::unlock(_lockCatalogue, lockHandle)) {
            // TODO: deferred unlocking
        }
    }

    Status ReplSetDistLockManager::checkStatus(const DistLockHandle& lockHandle) {
        // TODO: use catalogue to check.
        return Status::OK();
    }
}
