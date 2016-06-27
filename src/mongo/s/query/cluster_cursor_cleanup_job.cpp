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

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_cursor_cleanup_job.h"

#include "mongo/db/client.h"
#include "mongo/db/server_parameters.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/util/exit.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {

// Period of time after which mortal cursors are killed for inactivity. Configurable with server
// parameter "cursorTimeoutMillis".
std::atomic<long long> cursorTimeoutMillis(  // NOLINT
    durationCount<Milliseconds>(Minutes(10)));

ExportedServerParameter<long long, ServerParameterType::kStartupAndRuntime>
    cursorTimeoutMillisConfig(ServerParameterSet::getGlobal(),
                              "cursorTimeoutMillis",
                              &cursorTimeoutMillis);

// Frequency with which ClusterCursorCleanupJob is run.
MONGO_EXPORT_SERVER_PARAMETER(clientCursorMonitorFrequencySecs, long long, 4);

}  // namespace

ClusterCursorCleanupJob clusterCursorCleanupJob;

std::string ClusterCursorCleanupJob::name() const {
    return "ClusterCursorCleanupJob";
}

void ClusterCursorCleanupJob::run() {
    Client::initThread(name().c_str());
    ClusterCursorManager* manager = grid.getCursorManager();
    invariant(manager);

    while (!inShutdown()) {
        manager->killMortalCursorsInactiveSince(Date_t::now() -
                                                Milliseconds(cursorTimeoutMillis.load()));
        manager->incrementCursorsTimedOut(manager->reapZombieCursors());
        sleepsecs(clientCursorMonitorFrequencySecs);
    }
}

}  // namespace mongo
