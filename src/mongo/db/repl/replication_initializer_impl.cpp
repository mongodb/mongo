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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/repl/repl_coordinator_external_state_impl.h"
#include "mongo/db/repl/repl_coordinator_impl.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/network_interface_impl.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

MONGO_INITIALIZER_WITH_PREREQUISITES(CreateReplicationManager, ("SetGlobalEnvironment"))
        (InitializerContext* context) {
    repl::setGlobalReplicationCoordinator(new repl::ReplicationCoordinatorImpl(
            getGlobalReplSettings(),
            new repl::ReplicationCoordinatorExternalStateImpl,
            new repl::NetworkInterfaceImpl,
            new repl::TopologyCoordinatorImpl(Seconds(repl::maxSyncSourceLagSecs)),
            static_cast<int64_t>(curTimeMillis64())));
    return Status::OK();
}

} // namespace
} // namespace mongo
