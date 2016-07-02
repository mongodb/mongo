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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rs_sync.h"

#include <vector>

#include "third_party/murmurhash3/MurmurHash3.h"

#include "mongo/base/counter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/rs_initialsync.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

void runSyncThread(BackgroundSync* bgsync) {
    Client::initThread("rsSync");
    AuthorizationSession::get(cc())->grantInternalAuthorization();
    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();

    // Overwrite prefetch index mode in BackgroundSync if ReplSettings has a mode set.
    ReplSettings replSettings = replCoord->getSettings();
    if (replSettings.isPrefetchIndexModeSet())
        replCoord->setIndexPrefetchConfig(replSettings.getPrefetchIndexMode());

    while (!inShutdown()) {
        // After a reconfig, we may not be in the replica set anymore, so
        // check that we are in the set (and not an arbiter) before
        // trying to sync with other replicas.
        // TODO(spencer): Use a condition variable to await loading a config
        if (replCoord->getMemberState().startup()) {
            warning() << "did not receive a valid config yet";
            sleepsecs(1);
            continue;
        }

        const MemberState memberState = replCoord->getMemberState();

        // An arbiter can never transition to any other state, and doesn't replicate, ever
        if (memberState.arbiter()) {
            break;
        }

        // If we are removed then we don't belong to the set anymore
        if (memberState.removed()) {
            sleepsecs(5);
            continue;
        }

        try {
            if (memberState.primary() && !replCoord->isWaitingForApplierToDrain()) {
                sleepsecs(1);
                continue;
            }

            if (!replCoord->setFollowerMode(MemberState::RS_RECOVERING)) {
                continue;
            }

            /* we have some data.  continue tailing. */
            SyncTail tail(bgsync, multiSyncApply);
            tail.oplogApplication();
        } catch (...) {
            std::terminate();
        }
    }
}

}  // namespace repl
}  // namespace mongo
