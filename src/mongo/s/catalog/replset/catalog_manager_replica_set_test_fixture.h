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

#pragma once

#include <utility>

#include "mongo/executor/network_test_env.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class BSONObj;
class CatalogManagerReplicaSet;
class DistLockManagerMock;
struct RemoteCommandRequest;
class ShardRegistry;
template <typename T>
class StatusWith;

namespace executor {
class NetworkInterfaceMock;
}  // namespace executor

/**
 * Sets up the mocked out objects for testing the replica-set backed catalog manager.
 */
class CatalogManagerReplSetTestFixture : public mongo::unittest::Test {
public:
    CatalogManagerReplSetTestFixture();
    ~CatalogManagerReplSetTestFixture();

protected:
    template <typename Lambda>
    executor::NetworkTestEnv::FutureHandle<typename std::result_of<Lambda()>::type> launchAsync(
        Lambda&& func) const {
        return _networkTestEnv->launchAsync(std::forward<Lambda>(func));
    }

    CatalogManagerReplicaSet* catalogManager() const;

    ShardRegistry* shardRegistry() const;

    executor::NetworkInterfaceMock* network() const;

    DistLockManagerMock* distLock() const;

    /**
     * Blocking methods, which receive one message from the network and respond using the
     * responses returned from the input function. This is a syntactic sugar for simple,
     * single request + response or find tests.
     */
    void onCommand(executor::NetworkTestEnv::OnCommandFunction func);
    void onFindCommand(executor::NetworkTestEnv::OnFindCommandFunction func);

    void setUp() override;

    void tearDown() override;

    executor::NetworkInterfaceMock* _mockNetwork;
    std::unique_ptr<executor::NetworkTestEnv> _networkTestEnv;
};

}  // namespace mongo
