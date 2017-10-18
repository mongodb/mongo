/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/db/service_liason_mongos.h"

#include "mongo/db/service_context.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {

LogicalSessionIdSet ServiceLiasonMongos::getActiveOpSessions() const {
    LogicalSessionIdSet activeSessions;

    invariant(hasGlobalServiceContext());

    // Append any in-use session ids from the global cluster cursor managers.
    auto cursorManager = Grid::get(getGlobalServiceContext())->getCursorManager();
    cursorManager->appendActiveSessions(&activeSessions);

    return activeSessions;
}

LogicalSessionIdSet ServiceLiasonMongos::getOpenCursorSessions() const {
    LogicalSessionIdSet openCursorSessions;

    return openCursorSessions;
}

void ServiceLiasonMongos::scheduleJob(PeriodicRunner::PeriodicJob job) {
    invariant(hasGlobalServiceContext());
    getGlobalServiceContext()->getPeriodicRunner()->scheduleJob(std::move(job));
}

void ServiceLiasonMongos::join() {
    invariant(hasGlobalServiceContext());
    getGlobalServiceContext()->getPeriodicRunner()->shutdown();
}

Date_t ServiceLiasonMongos::now() const {
    invariant(hasGlobalServiceContext());
    return getGlobalServiceContext()->getFastClockSource()->now();
}

ServiceContext* ServiceLiasonMongos::_context() {
    return getGlobalServiceContext();
}

std::pair<Status, int> ServiceLiasonMongos::killCursorsWithMatchingSessions(
    OperationContext* opCtx, const SessionKiller::Matcher& matcher) {
    auto cursorManager = Grid::get(getGlobalServiceContext())->getCursorManager();
    return cursorManager->killCursorsWithMatchingSessions(opCtx, matcher);
}

}  // namespace mongo
