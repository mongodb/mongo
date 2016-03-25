/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/local_client.h"

#include <boost/optional.hpp>

#include "mongo/base/checked_cast.h"
#include "mongo/db/client.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

class LocalClientTest : public unittest::Test {
protected:
    ServiceContext::UniqueOperationContext _txn;

private:
    void setUp() override;
    void tearDown() override;
};

void LocalClientTest::setUp() {
    ServiceContext* serviceContext = getGlobalServiceContext();
    if (!serviceContext->getGlobalStorageEngine()) {
        // When using the 'devnull' storage engine, it is fine for the temporary directory to
        // go away after the global storage engine is initialized.
        unittest::TempDir tempDir("local_client_test");
        storageGlobalParams.dbpath = tempDir.path();
        storageGlobalParams.dbpath = tempDir.path();
        storageGlobalParams.engine = "ephemeralForTest";
        storageGlobalParams.engineSetByUser = true;
        checked_cast<ServiceContextMongoD*>(getGlobalServiceContext())->createLockFile();
        serviceContext->initializeGlobalStorageEngine();
    }
    Client::initThreadIfNotAlready();
    _txn = serviceContext->makeOperationContext(&cc());

    const repl::ReplSettings replSettings = {};
    repl::setGlobalReplicationCoordinator(new repl::ReplicationCoordinatorMock(replSettings));
}

void LocalClientTest::tearDown() {
    {
        Lock::GlobalWrite globalLock(_txn->lockState());
        BSONObjBuilder unused;
        invariant(dbHolder().closeAll(_txn.get(), unused, false));
    }
    _txn.reset();
    repl::setGlobalReplicationCoordinator(nullptr);
}

/**
 * Sets up a collection "nss" and populates it with documents.
 */
void setUpCollection(OperationContext* txn,
                     const NamespaceString& nss,
                     std::vector<BSONObj> documentVector) {
    AutoGetOrCreateDb autoDb(txn, nss.db(), MODE_X);
    WriteUnitOfWork wuow(txn);
    auto coll = autoDb.getDb()->getCollection(nss.ns());
    if (!coll) {
        coll = autoDb.getDb()->createCollection(txn, nss.ns());
    }
    ASSERT(coll);
    for (const BSONObj& doc : documentVector) {
        ASSERT_OK(coll->insertDocument(txn, doc, false));
    }
    wuow.commit();
}

/**
 * Drops collection "nss".
 */
void dropCollection(OperationContext* txn, const NamespaceString& nss) {
    AutoGetOrCreateDb autoDb(txn, nss.db(), MODE_X);
    ASSERT_EQUALS(Status::OK(), autoDb.getDb()->dropCollection(txn, nss.ns()));
}

// Successful ordered find query.
TEST_F(LocalClientTest, FindQuerySuccessful) {
    LocalClient localClient(_txn.get());
    NamespaceString nss("test.foo");
    BSONObj firstDocument = BSON("_id" << 1 << "v" << 2);
    BSONObj secondDocument = BSON("_id" << 4 << "v" << 2);
    std::vector<BSONObj> documentVector = {firstDocument, secondDocument};
    setUpCollection(_txn.get(), nss, documentVector);

    // Set up find query that matches documents in the collection.
    StatusWith<LocalClient::LocalCursor> localCursor =
        localClient.query(nss, BSON("v" << 2), BSON("_id" << 1));
    ASSERT_OK(localCursor.getStatus());

    // Retrieve the first document and check it.
    boost::optional<BSONObj> document = assertGet(localCursor.getValue().next());
    ASSERT_TRUE(document);
    ASSERT_EQUALS(*document, firstDocument);

    // Retrieve the second document and check it.
    document = assertGet(localCursor.getValue().next());
    ASSERT_TRUE(document);
    ASSERT_EQUALS(*document, secondDocument);

    // Check that the cursor is exhausted: no more documents to retrieve.
    document = assertGet(localCursor.getValue().next());
    ASSERT_FALSE(document);

    dropCollection(_txn.get(), nss);
}

// No documents match the find query.
TEST_F(LocalClientTest, FindQueryNoDocumentsFound) {
    LocalClient localClient(_txn.get());
    NamespaceString nss("test.foo");
    std::vector<BSONObj> documentVector = {BSON("_id" << 1 << "v" << 2)};
    setUpCollection(_txn.get(), nss, documentVector);

    // Set up find query that does not match any documents in the collection.
    StatusWith<LocalClient::LocalCursor> localCursor =
        localClient.query(nss, BSON("v" << 4), BSON("_id" << 1));
    ASSERT_OK(localCursor.getStatus());

    // Request the next document and receive nothing, with no errors.
    boost::optional<BSONObj> document = assertGet(localCursor.getValue().next());
    ASSERT_FALSE(document);

    dropCollection(_txn.get(), nss);
}

// Collection being queried does not exist.
TEST_F(LocalClientTest, FindQueryInitNoCollection) {
    LocalClient localClient(_txn.get());

    // Set up find query on a non-existent collection.
    StatusWith<LocalClient::LocalCursor> localCursor =
        localClient.query(NamespaceString("test.bam"), BSON("v" << 4), BSON("_id" << 1));
    ASSERT_OK(localCursor.getStatus());

    // Request the next document and receive nothing, with no errors.
    boost::optional<BSONObj> document = assertGet(localCursor.getValue().next());
    ASSERT_FALSE(document);
}

}  // namespace
}  // namespace mongo
