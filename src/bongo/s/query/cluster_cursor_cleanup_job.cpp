/**
 *    Copyright (C) 2015 BongoDB Inc.
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

#include "bongo/platform/basic.h"

#include "bongo/s/query/cluster_cursor_cleanup_job.h"

#include "bongo/db/client.h"
#include "bongo/db/server_parameters.h"
#include "bongo/s/grid.h"
#include "bongo/s/query/cluster_cursor_manager.h"
#include "bongo/util/exit.h"
#include "bongo/util/time_support.h"

namespace bongo {

namespace {

// Period of time after which mortal cursors are killed for inactivity. Configurable with server
// parameter "cursorTimeoutMillis".
AtomicInt64 cursorTimeoutMillis(durationCount<Milliseconds>(Minutes(10)));

ExportedServerParameter<long long, ServerParameterType::kStartupAndRuntime>
    cursorTimeoutMillisConfig(ServerParameterSet::getGlobal(),
                              "cursorTimeoutMillis",
                              &cursorTimeoutMillis);

// Frequency with which ClusterCursorCleanupJob is run.
BONGO_EXPORT_SERVER_PARAMETER(clientCursorMonitorFrequencySecs, long long, 4);

}  // namespace

ClusterCursorCleanupJob clusterCursorCleanupJob;

std::string ClusterCursorCleanupJob::name() const {
    return "ClusterCursorCleanupJob";
}

void ClusterCursorCleanupJob::run() {
    Client::initThread(name().c_str());
    ClusterCursorManager* manager = grid.getCursorManager();
    invariant(manager);

    while (!globalInShutdownDeprecated()) {
        manager->killMortalCursorsInactiveSince(Date_t::now() -
                                                Milliseconds(cursorTimeoutMillis.load()));
        manager->incrementCursorsTimedOut(manager->reapZombieCursors());
        sleepsecs(clientCursorMonitorFrequencySecs.load());
    }
}

}  // namespace bongo
