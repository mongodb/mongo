/**
 *    Copyright 2017 MongoDB Inc.
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

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/rollback_fix_up_info.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

/**
 * Creates ReplSettings for ReplicationCoordinatorMock.
 */
ReplSettings createReplSettings() {
    ReplSettings settings;
    settings.setOplogSizeBytes(5 * 1024 * 1024);
    settings.setReplSetString("mySet/node1:12345");
    return settings;
}

/**
 * Creates an OperationContext using the current Client.
 */
ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

class RollbackFixUpInfoTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    /**
     * Check collection contents against given vector of documents.
     * Ordering of documents in collection does not need to match order in provided vector.
     */
    void _assertDocumentsInCollectionEquals(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            std::initializer_list<BSONObj> expectedDocs);

    std::unique_ptr<StorageInterface> _storageInterface;
};

void RollbackFixUpInfoTest::setUp() {
    ServiceContextMongoDTest::setUp();
    _storageInterface = stdx::make_unique<StorageInterfaceImpl>();
    auto serviceContext = getServiceContext();
    ReplicationCoordinator::set(
        serviceContext,
        stdx::make_unique<ReplicationCoordinatorMock>(serviceContext, createReplSettings()));

    auto opCtx = makeOpCtx();
    ASSERT_OK(_storageInterface->createCollection(
        opCtx.get(), RollbackFixUpInfo::kRollbackDocsNamespace, {}));
    ASSERT_OK(_storageInterface->createCollection(
        opCtx.get(), RollbackFixUpInfo::kRollbackCollectionUuidNamespace, {}));
}

void RollbackFixUpInfoTest::tearDown() {
    _storageInterface = {};
    ServiceContextMongoDTest::tearDown();
}

/**
 * Returns string representation of a vector of BSONObj.
 */
template <typename T>
std::string _toString(const T& docs) {
    str::stream ss;
    ss << "[";
    bool first = true;
    for (const auto& doc : docs) {
        if (first) {
            ss << doc;
            first = false;
        } else {
            ss << ", " << doc;
        }
    }
    ss << "]";
    return ss;
}

void RollbackFixUpInfoTest::_assertDocumentsInCollectionEquals(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::initializer_list<BSONObj> expectedDocs) {
    auto indexName = "_id_"_sd;
    const auto actualDocs = unittest::assertGet(
        _storageInterface->findDocuments(opCtx,
                                         nss,
                                         indexName,
                                         StorageInterface::ScanDirection::kForward,
                                         {},
                                         BoundInclusion::kIncludeStartKeyOnly,
                                         10000U));
    std::string msg = str::stream() << "expected: " << _toString(expectedDocs)
                                    << "; actual: " << _toString(actualDocs);
    ASSERT_EQUALS(expectedDocs.size(), actualDocs.size()) << msg;

    auto unorderedExpectedDocsSet =
        mongo::SimpleBSONObjComparator::kInstance.makeBSONObjUnorderedSet(expectedDocs);
    for (const auto& doc : actualDocs) {
        std::string docMsg = str::stream() << "Unexpected document " << doc << " in collection "
                                           << nss.ns() << ": " << msg;
        ASSERT_TRUE(unorderedExpectedDocsSet.find(doc) != unorderedExpectedDocsSet.end()) << docMsg;
    }
}

TEST_F(RollbackFixUpInfoTest,
       ProcessInsertDocumentOplogEntryInsertsDocumentIntoRollbackDocsCollectionWithInsertOpType) {
    auto operation = BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL << "op"
                               << "i"
                               << "ns"
                               << "test.t"
                               << "ui"
                               << UUID::gen().toBSON().firstElement()
                               << "o"
                               << BSON("_id"
                                       << "mydocid"
                                       << "a"
                                       << 1));

    auto collectionUuid = unittest::assertGet(UUID::parse(operation["ui"]));
    auto docId = operation["o"].Obj()["_id"];

    auto opCtx = makeOpCtx();
    RollbackFixUpInfo rollbackFixUpInfo(_storageInterface.get());
    ASSERT_OK(rollbackFixUpInfo.processSingleDocumentOplogEntry(
        opCtx.get(), collectionUuid, docId, RollbackFixUpInfo::SingleDocumentOpType::kInsert));

    auto expectedDocument = BSON(
        "_id" << BSON("collectionUuid" << collectionUuid.toBSON().firstElement() << "documentId"
                                       << docId)
              << "operationType"
              << "insert"
              << "documentToRestore"
              << BSONNULL);
    _assertDocumentsInCollectionEquals(
        opCtx.get(), RollbackFixUpInfo::kRollbackDocsNamespace, {expectedDocument});
}

TEST_F(RollbackFixUpInfoTest,
       ProcessDeleteDocumentOplogEntryInsertsDocumentIntoRollbackDocsCollectionWithDeleteOpType) {
    auto operation = BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL << "op"
                               << "d"
                               << "ns"
                               << "test.t"
                               << "ui"
                               << UUID::gen().toBSON().firstElement()
                               << "o"
                               << BSON("_id"
                                       << "mydocid"));

    auto collectionUuid = unittest::assertGet(UUID::parse(operation["ui"]));
    auto docId = operation["o"].Obj()["_id"];

    auto opCtx = makeOpCtx();
    RollbackFixUpInfo rollbackFixUpInfo(_storageInterface.get());
    ASSERT_OK(rollbackFixUpInfo.processSingleDocumentOplogEntry(
        opCtx.get(), collectionUuid, docId, RollbackFixUpInfo::SingleDocumentOpType::kDelete));

    auto expectedDocument = BSON(
        "_id" << BSON("collectionUuid" << collectionUuid.toBSON().firstElement() << "documentId"
                                       << docId)
              << "operationType"
              << "delete"
              << "documentToRestore"
              << BSONNULL);

    _assertDocumentsInCollectionEquals(
        opCtx.get(), RollbackFixUpInfo::kRollbackDocsNamespace, {expectedDocument});
}

TEST_F(RollbackFixUpInfoTest,
       ProcessUpdateDocumentOplogEntryInsertsDocumentIntoRollbackDocsCollectionWithUpdateOpType) {
    auto operation = BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL << "op"
                               << "d"
                               << "ns"
                               << "test.t"
                               << "ui"
                               << UUID::gen().toBSON().firstElement()
                               << "o2"
                               << BSON("_id"
                                       << "mydocid")
                               << "o"
                               << BSON("$set" << BSON("x" << 2)));

    auto collectionUuid = unittest::assertGet(UUID::parse(operation["ui"]));
    auto docId = operation["o2"].Obj()["_id"];

    auto opCtx = makeOpCtx();
    RollbackFixUpInfo rollbackFixUpInfo(_storageInterface.get());
    ASSERT_OK(rollbackFixUpInfo.processSingleDocumentOplogEntry(
        opCtx.get(), collectionUuid, docId, RollbackFixUpInfo::SingleDocumentOpType::kUpdate));

    auto expectedDocument = BSON(
        "_id" << BSON("collectionUuid" << collectionUuid.toBSON().firstElement() << "documentId"
                                       << docId)
              << "operationType"
              << "update"
              << "documentToRestore"
              << BSONNULL);

    _assertDocumentsInCollectionEquals(
        opCtx.get(), RollbackFixUpInfo::kRollbackDocsNamespace, {expectedDocument});
}

TEST_F(
    RollbackFixUpInfoTest,
    ProcessCreateCollectionOplogEntryInsertsDocumentIntoRollbackCollectionUuidCollectionWithEmptyNamespace) {
    auto operation = BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL << "op"
                               << "c"
                               << "ns"
                               << "mydb.$cmd"
                               << "ui"
                               << UUID::gen().toBSON().firstElement()
                               << "o"
                               << BSON("create"
                                       << "mynewcoll"
                                       << "idIndex"
                                       << BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                                   << "_id_"
                                                   << "ns"
                                                   << "mydb.mynewcoll")));
    auto collectionUuid = unittest::assertGet(UUID::parse(operation["ui"]));
    NamespaceString commandNss(operation["ns"].String());
    auto collectionName = operation["o"].Obj().firstElement().String();

    ASSERT_TRUE(OplogEntry(operation).isCommand());

    auto opCtx = makeOpCtx();
    RollbackFixUpInfo rollbackFixUpInfo(_storageInterface.get());
    ASSERT_OK(rollbackFixUpInfo.processCreateCollectionOplogEntry(opCtx.get(), collectionUuid));

    auto expectedDocument = BSON("_id" << collectionUuid.toBSON().firstElement() << "ns"
                                       << "");

    _assertDocumentsInCollectionEquals(
        opCtx.get(), RollbackFixUpInfo::kRollbackCollectionUuidNamespace, {expectedDocument});
}

TEST_F(RollbackFixUpInfoTest,
       ProcessDropCollectionOplogEntryInsertsDocumentIntoRollbackCollectionUuidCollection) {
    auto operation = BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL << "op"
                               << "c"
                               << "ns"
                               << "mydb.$cmd"
                               << "ui"
                               << UUID::gen().toBSON().firstElement()
                               << "o"
                               << BSON("drop"
                                       << "mydroppedcoll"));
    auto collectionUuid = unittest::assertGet(UUID::parse(operation["ui"]));
    NamespaceString commandNss(operation["ns"].String());
    auto collectionName = operation["o"].Obj().firstElement().String();
    NamespaceString nss(commandNss.db(), collectionName);

    ASSERT_TRUE(OplogEntry(operation).isCommand());

    auto opCtx = makeOpCtx();
    RollbackFixUpInfo rollbackFixUpInfo(_storageInterface.get());
    ASSERT_OK(rollbackFixUpInfo.processDropCollectionOplogEntry(opCtx.get(), collectionUuid, nss));

    auto expectedDocument =
        BSON("_id" << collectionUuid.toBSON().firstElement() << "ns" << nss.ns());

    _assertDocumentsInCollectionEquals(
        opCtx.get(), RollbackFixUpInfo::kRollbackCollectionUuidNamespace, {expectedDocument});
}

TEST_F(
    RollbackFixUpInfoTest,
    ProcessRenameCollectionOplogEntryWithDropTargetFalseInsertsOneDocumentIntoRollbackCollectionUuidCollection) {
    auto operation = BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL << "op"
                               << "c"
                               << "ns"
                               << "mydb.$cmd"
                               << "ui"
                               << UUID::gen().toBSON().firstElement()
                               << "o"
                               << BSON("renameCollection"
                                       << "mydb.prevCollName"
                                       << "to"
                                       << "mydb.newCollName"
                                       << "stayTemp"
                                       << false
                                       << "dropTarget"
                                       << false));
    auto collectionUuid = unittest::assertGet(UUID::parse(operation["ui"]));
    NamespaceString sourceNss(operation["o"].Obj().firstElement().String());
    ASSERT_EQUALS(ErrorCodes::UnknownError, UUID::parse(operation["o"].Obj()["dropTarget"]));

    ASSERT_TRUE(OplogEntry(operation).isCommand());

    auto opCtx = makeOpCtx();
    RollbackFixUpInfo rollbackFixUpInfo(_storageInterface.get());
    ASSERT_OK(rollbackFixUpInfo.processRenameCollectionOplogEntry(
        opCtx.get(), collectionUuid, sourceNss, boost::none));

    auto expectedDocument =
        BSON("_id" << collectionUuid.toBSON().firstElement() << "ns" << sourceNss.ns());

    _assertDocumentsInCollectionEquals(
        opCtx.get(), RollbackFixUpInfo::kRollbackCollectionUuidNamespace, {expectedDocument});
}

TEST_F(
    RollbackFixUpInfoTest,
    ProcessRenameCollectionOplogEntryWithValidDropTargetUuidInsertsTwoDocumentsIntoRollbackCollectionUuidCollection) {
    auto operation = BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL << "op"
                               << "c"
                               << "ns"
                               << "mydb.$cmd"
                               << "ui"
                               << UUID::gen().toBSON().firstElement()
                               << "o"
                               << BSON("renameCollection"
                                       << "mydb.prevCollName"
                                       << "to"
                                       << "mydb.newCollName"
                                       << "stayTemp"
                                       << false
                                       << "dropTarget"
                                       << UUID::gen().toBSON().firstElement()));
    auto collectionUuid = unittest::assertGet(UUID::parse(operation["ui"]));
    NamespaceString sourceNss(operation["o"].Obj().firstElement().String());
    NamespaceString targetNss(operation["o"].Obj()["to"].String());
    auto droppedCollectionUuid =
        unittest::assertGet(UUID::parse(operation["o"].Obj()["dropTarget"]));

    ASSERT_TRUE(OplogEntry(operation).isCommand());

    auto opCtx = makeOpCtx();
    RollbackFixUpInfo rollbackFixUpInfo(_storageInterface.get());
    ASSERT_OK(rollbackFixUpInfo.processRenameCollectionOplogEntry(
        opCtx.get(), collectionUuid, sourceNss, std::make_pair(droppedCollectionUuid, targetNss)));

    auto expectedDocument1 =
        BSON("_id" << collectionUuid.toBSON().firstElement() << "ns" << sourceNss.ns());
    auto expectedDocument2 =
        BSON("_id" << droppedCollectionUuid.toBSON().firstElement() << "ns" << targetNss.ns());

    _assertDocumentsInCollectionEquals(opCtx.get(),
                                       RollbackFixUpInfo::kRollbackCollectionUuidNamespace,
                                       {expectedDocument1, expectedDocument2});
}

}  // namespace
