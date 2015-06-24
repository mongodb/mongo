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

#include "mongo/db/repl/base_cloner_test_fixture.h"

#include <memory>

#include "mongo/stdx/thread.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {

const HostAndPort BaseClonerTest::target("localhost", -1);
const NamespaceString BaseClonerTest::nss("db.coll");
const BSONObj BaseClonerTest::idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                                     << "_id_"
                                                     << "ns" << nss.ns());

// static
BSONObj BaseClonerTest::createCursorResponse(CursorId cursorId,
                                             const std::string& ns,
                                             const BSONArray& docs,
                                             const char* batchFieldName) {
    return BSON("cursor" << BSON("id" << cursorId << "ns" << ns << batchFieldName << docs) << "ok"
                         << 1);
}

// static
BSONObj BaseClonerTest::createCursorResponse(CursorId cursorId,
                                             const BSONArray& docs,
                                             const char* batchFieldName) {
    return createCursorResponse(cursorId, nss.toString(), docs, batchFieldName);
}

// static
BSONObj BaseClonerTest::createCursorResponse(CursorId cursorId, const BSONArray& docs) {
    return createCursorResponse(cursorId, docs, "firstBatch");
}

// static
BSONObj BaseClonerTest::createListCollectionsResponse(CursorId cursorId,
                                                      const BSONArray& colls,
                                                      const char* fieldName) {
    return createCursorResponse(cursorId, "test.$cmd.listCollections.coll", colls, fieldName);
}

// static
BSONObj BaseClonerTest::createListCollectionsResponse(CursorId cursorId, const BSONArray& colls) {
    return createListCollectionsResponse(cursorId, colls, "firstBatch");
}

// static
BSONObj BaseClonerTest::createListIndexesResponse(CursorId cursorId,
                                                  const BSONArray& specs,
                                                  const char* batchFieldName) {
    return createCursorResponse(cursorId, "test.$cmd.listIndexes.coll", specs, batchFieldName);
}

// static
BSONObj BaseClonerTest::createListIndexesResponse(CursorId cursorId, const BSONArray& specs) {
    return createListIndexesResponse(cursorId, specs, "firstBatch");
}

BaseClonerTest::BaseClonerTest()
    : _mutex(), _setStatusCondition(), _status(getDetectableErrorStatus()) {}

void BaseClonerTest::setUp() {
    ReplicationExecutorTest::setUp();
    clear();
    launchExecutorThread();
    storageInterface.reset(new ClonerStorageInterfaceMock());
}

void BaseClonerTest::tearDown() {
    ReplicationExecutorTest::tearDown();
    storageInterface.reset();
}

void BaseClonerTest::clear() {
    _status = getDetectableErrorStatus();
}

void BaseClonerTest::setStatus(const Status& status) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _status = status;
    _setStatusCondition.notify_all();
}

const Status& BaseClonerTest::getStatus() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _status;
}

void BaseClonerTest::scheduleNetworkResponse(NetworkOperationIterator noi, const BSONObj& obj) {
    auto net = getNet();
    Milliseconds millis(0);
    RemoteCommandResponse response(obj, BSONObj(), millis);
    ReplicationExecutor::ResponseStatus responseStatus(response);
    net->scheduleResponse(noi, net->now(), responseStatus);
}

void BaseClonerTest::scheduleNetworkResponse(NetworkOperationIterator noi,
                                             ErrorCodes::Error code,
                                             const std::string& reason) {
    auto net = getNet();
    ReplicationExecutor::ResponseStatus responseStatus(code, reason);
    net->scheduleResponse(noi, net->now(), responseStatus);
}

void BaseClonerTest::scheduleNetworkResponse(const BSONObj& obj) {
    ASSERT_TRUE(getNet()->hasReadyRequests());
    scheduleNetworkResponse(getNet()->getNextReadyRequest(), obj);
}

void BaseClonerTest::scheduleNetworkResponse(ErrorCodes::Error code, const std::string& reason) {
    ASSERT_TRUE(getNet()->hasReadyRequests());
    scheduleNetworkResponse(getNet()->getNextReadyRequest(), code, reason);
}

void BaseClonerTest::processNetworkResponse(const BSONObj& obj) {
    scheduleNetworkResponse(obj);
    finishProcessingNetworkResponse();
}

void BaseClonerTest::processNetworkResponse(ErrorCodes::Error code, const std::string& reason) {
    scheduleNetworkResponse(code, reason);
    finishProcessingNetworkResponse();
}

void BaseClonerTest::finishProcessingNetworkResponse() {
    clear();
    getNet()->runReadyNetworkOperations();
}

void BaseClonerTest::testLifeCycle() {
    // GetDiagnosticString
    ASSERT_FALSE(getCloner()->getDiagnosticString().empty());

    // IsActiveAfterStart
    ASSERT_FALSE(getCloner()->isActive());
    ASSERT_OK(getCloner()->start());
    ASSERT_TRUE(getCloner()->isActive());
    tearDown();

    // StartWhenActive
    setUp();
    ASSERT_OK(getCloner()->start());
    ASSERT_TRUE(getCloner()->isActive());
    ASSERT_NOT_OK(getCloner()->start());
    ASSERT_TRUE(getCloner()->isActive());
    tearDown();

    // CancelWithoutStart
    setUp();
    ASSERT_FALSE(getCloner()->isActive());
    getCloner()->cancel();
    ASSERT_FALSE(getCloner()->isActive());
    tearDown();

    // WaitWithoutStart
    setUp();
    ASSERT_FALSE(getCloner()->isActive());
    getCloner()->wait();
    ASSERT_FALSE(getCloner()->isActive());
    tearDown();

    // ShutdownBeforeStart
    setUp();
    getExecutor().shutdown();
    ASSERT_NOT_OK(getCloner()->start());
    ASSERT_FALSE(getCloner()->isActive());
    tearDown();

    // StartAndCancel
    setUp();
    ASSERT_OK(getCloner()->start());
    scheduleNetworkResponse(BSON("ok" << 1));
    getCloner()->cancel();
    finishProcessingNetworkResponse();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus().code());
    ASSERT_FALSE(getCloner()->isActive());
    tearDown();

    // StartButShutdown
    setUp();
    ASSERT_OK(getCloner()->start());
    scheduleNetworkResponse(BSON("ok" << 1));
    getExecutor().shutdown();
    // Network interface should not deliver mock response to callback.
    finishProcessingNetworkResponse();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus().code());
    ASSERT_FALSE(getCloner()->isActive());
}

Status ClonerStorageInterfaceMock::beginCollection(OperationContext* txn,
                                                   const NamespaceString& nss,
                                                   const CollectionOptions& options,
                                                   const std::vector<BSONObj>& specs) {
    return beginCollectionFn ? beginCollectionFn(txn, nss, options, specs) : Status::OK();
}

Status ClonerStorageInterfaceMock::insertDocuments(OperationContext* txn,
                                                   const NamespaceString& nss,
                                                   const std::vector<BSONObj>& docs) {
    return insertDocumentsFn ? insertDocumentsFn(txn, nss, docs) : Status::OK();
}

Status ClonerStorageInterfaceMock::commitCollection(OperationContext* txn,
                                                    const NamespaceString& nss) {
    return Status::OK();
}

Status ClonerStorageInterfaceMock::insertMissingDoc(OperationContext* txn,
                                                    const NamespaceString& nss,
                                                    const BSONObj& doc) {
    return Status::OK();
}

Status ClonerStorageInterfaceMock::dropUserDatabases(OperationContext* txn) {
    return dropUserDatabasesFn ? dropUserDatabasesFn(txn) : Status::OK();
}

}  // namespace repl
}  // namespace mongo
