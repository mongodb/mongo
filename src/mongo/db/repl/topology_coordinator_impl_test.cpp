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

#include <limits>

#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

namespace {

    ReplicaSetConfig startConfig;

    TEST(TopologyCoordinator, ForceSyncSource) {
        TopologyCoordinatorImpl topocoord(std::numeric_limits<int>::max());
        Date_t now = 0;
        topocoord.updateConfig(startConfig, 0, now++);
        topocoord.chooseNewSyncSource(now++);
        // ASSERT(getSyncSourceAddress() == HostAndPort(expected one);
        topocoord.setForceSyncSourceIndex(2);
        topocoord.chooseNewSyncSource(now++);
        // ASSERT(getSyncSourceAddress() == HostAndPort(expected one / id #2);
    }

    TEST(TopologyCoordinator, BlacklistSyncSource) {
        TopologyCoordinatorImpl topocoord(std::numeric_limits<int>::max());
        Date_t now = 0;
        topocoord.updateConfig(startConfig, 0, now++);
        topocoord.chooseNewSyncSource(now++);
        // ASSERT(getSyncSourceAddress() == HostAndPort(expected one);
        
        Date_t expireTime = 100;
        topocoord.blacklistSyncSource(HostAndPort() /* the current one */, expireTime);
        topocoord.chooseNewSyncSource(now++);
        // ASSERT(getSyncSourceAddress() == HostAndPort(expected one);
        topocoord.chooseNewSyncSource(expireTime);
        // ASSERT(getSyncSourceAddress() == HostAndPort(blacklisted one);
    }

}  // namespace
}  // namespace repl
}  // namespace mongo
