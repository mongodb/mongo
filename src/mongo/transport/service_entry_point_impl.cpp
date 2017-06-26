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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/transport/service_entry_point_impl.h"

#include <vector>

#include "mongo/transport/service_entry_point_utils.h"
#include "mongo/transport/service_state_machine.h"
#include "mongo/transport/session.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

void ServiceEntryPointImpl::startSession(transport::SessionHandle session) {
    // Pass ownership of the transport::SessionHandle into our worker thread. When this
    // thread exits, the session will end.
    launchServiceWorkerThread([ this, session = std::move(session) ]() mutable {
        _nWorkers.addAndFetch(1);
        const auto guard = MakeGuard([this] { _nWorkers.subtractAndFetch(1); });

        ServiceStateMachine ssm(_svcCtx, std::move(session), true);
        const auto numCores = [] {
            ProcessInfo p;
            if (auto availCores = p.getNumAvailableCores()) {
                return static_cast<unsigned>(*availCores);
            }
            return static_cast<unsigned>(p.getNumCores());
        }();

        while (ssm.state() != ServiceStateMachine::State::Ended) {
            ssm.runNext();
            if (_nWorkers.load() > numCores)
                stdx::this_thread::yield();
        }
    });
}

}  // namespace mongo
