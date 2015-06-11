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

#include "mongo/s/catalog/replset/catalog_manager_replica_set_test_fixture.h"

#include <vector>

#include "mongo/client/remote_command_runner_mock.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/dist_lock_catalog_mock.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/replset_dist_lock_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"

namespace mongo {

    using std::vector;

    CatalogManagerReplSetTestFixture::CatalogManagerReplSetTestFixture() {
        std::unique_ptr<CatalogManagerReplicaSet> cm(
            stdx::make_unique<CatalogManagerReplicaSet>());

        std::unique_ptr<ReplSetDistLockManager> distLockMgr(
            stdx::make_unique<ReplSetDistLockManager>("Test",
                                                      stdx::make_unique<DistLockCatalogMock>(),
                                                      Milliseconds(2),
                                                      Seconds(10)));

        ASSERT_OK(cm->init(ConnectionString::forReplicaSet("CatalogManagerReplSetTest",
                                                           { HostAndPort{ "TestHost1" },
                                                             HostAndPort{ "TestHost2" } }),
                           std::move(distLockMgr)));

        std::unique_ptr<ShardRegistry> shardRegistry(
            stdx::make_unique<ShardRegistry>(stdx::make_unique<RemoteCommandTargeterFactoryMock>(),
                                             stdx::make_unique<RemoteCommandRunnerMock>(),
                                             std::unique_ptr<executor::TaskExecutor>{nullptr},
                                             cm.get()));

        // For now initialize the global grid object. All sharding objects will be accessible
        // from there until we get rid of it.
        grid.init(std::move(cm), std::move(shardRegistry));
    }

    CatalogManagerReplSetTestFixture::~CatalogManagerReplSetTestFixture() {
        grid.clearForUnitTests();
    }

    CatalogManagerReplicaSet* CatalogManagerReplSetTestFixture::catalogManager() const {
        auto cm = dynamic_cast<CatalogManagerReplicaSet*>(grid.catalogManager());
        invariant(cm);

        return cm;
    }

    ShardRegistry* CatalogManagerReplSetTestFixture::shardRegistry() const {
        return grid.shardRegistry();
    }

    RemoteCommandRunnerMock* CatalogManagerReplSetTestFixture::commandRunner() const {
        return RemoteCommandRunnerMock::get(shardRegistry()->getCommandRunner());
    }

} // namespace mongo
