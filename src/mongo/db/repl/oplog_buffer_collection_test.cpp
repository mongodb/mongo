/**
 *    Copyright 2015 MongoDB Inc.
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

#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

class OplogBufferCollectionTest : public ServiceContextMongoDTest {
protected:
    Client* getClient() const;

protected:
    ServiceContext::UniqueOperationContext makeOperationContext() const;

    ServiceContext::UniqueOperationContext _txn;

private:
    void setUp() override;
    void tearDown() override;
};

void OplogBufferCollectionTest::setUp() {
    ServiceContextMongoDTest::setUp();

    // Initializes cc() used in ServiceContextMongoD::_newOpCtx().
    Client::initThreadIfNotAlready("OplogBufferCollectionTest");

    auto serviceContext = getGlobalServiceContext();

    // AutoGetCollectionForRead requires a valid replication coordinator in order to check the shard
    // version.
    ReplSettings replSettings;
    replSettings.setOplogSizeBytes(5 * 1024 * 1024);
    ReplicationCoordinator::set(serviceContext,
                                stdx::make_unique<ReplicationCoordinatorMock>(replSettings));

    StorageInterface::set(serviceContext, stdx::make_unique<StorageInterfaceImpl>());

    _txn = makeOperationContext();
}

void OplogBufferCollectionTest::tearDown() {
    _txn.reset();

    ServiceContextMongoDTest::tearDown();
}

ServiceContext::UniqueOperationContext OplogBufferCollectionTest::makeOperationContext() const {
    return cc().makeOperationContext();
}

Client* OplogBufferCollectionTest::getClient() const {
    return &cc();
}

/**
 * Generates a unique namespace from the test registration agent.
 */
template <typename T>
NamespaceString makeNamespace(const T& t, const char* suffix = "") {
    return NamespaceString("local." + t.getSuiteName() + "_" + t.getTestName() + suffix);
}

TEST_F(OplogBufferCollectionTest, DefaultNamespace) {
    ASSERT_EQUALS(OplogBufferCollection::getDefaultNamespace(),
                  OplogBufferCollection().getNamespace());
}

TEST_F(OplogBufferCollectionTest, GetNamespace) {
    auto nss = makeNamespace(_agent);
    ASSERT_EQUALS(nss, OplogBufferCollection(nss).getNamespace());
}

TEST_F(OplogBufferCollectionTest, StartupCreatesCollection) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    // Collection should not exist until startup() is called.
    ASSERT_FALSE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());

    oplogBuffer.startup(_txn.get());
    ASSERT_TRUE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());
}

}  // namespace
