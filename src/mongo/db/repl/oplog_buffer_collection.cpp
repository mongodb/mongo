/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/repl/oplog_buffer_collection.h"

#include <algorithm>
#include <iterator>
#include <numeric>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {

const char kDefaultOplogCollectionNamespace[] = "local.temp_oplog_buffer";
const char kOplogEntryFieldName[] = "entry";

}  // namespace

NamespaceString OplogBufferCollection::getDefaultNamespace() {
    return NamespaceString(kDefaultOplogCollectionNamespace);
}

BSONObj OplogBufferCollection::addIdToDocument(const BSONObj& orig) {
    BSONObjBuilder bob;
    bob.append("_id", orig["ts"].timestamp());
    bob.append(kOplogEntryFieldName, orig);
    return bob.obj();
}

BSONObj OplogBufferCollection::extractEmbeddedOplogDocument(const BSONObj& orig) {
    return orig.getObjectField(kOplogEntryFieldName);
}


OplogBufferCollection::OplogBufferCollection(StorageInterface* storageInterface)
    : OplogBufferCollection(storageInterface, getDefaultNamespace()) {}

OplogBufferCollection::OplogBufferCollection(StorageInterface* storageInterface,
                                             const NamespaceString& nss)
    : _storageInterface(storageInterface), _nss(nss), _count(0), _size(0) {}

NamespaceString OplogBufferCollection::getNamespace() const {
    return _nss;
}

void OplogBufferCollection::startup(OperationContext* txn) {
    _createCollection(txn);
}

void OplogBufferCollection::shutdown(OperationContext* txn) {
    _dropCollection(txn);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _size = 0;
    _count = 0;
}

void OplogBufferCollection::pushEvenIfFull(OperationContext* txn, const Value& value) {
    Batch valueBatch = {value};
    pushAllNonBlocking(txn, valueBatch.begin(), valueBatch.end());
}

void OplogBufferCollection::push(OperationContext* txn, const Value& value) {
    pushEvenIfFull(txn, value);
}

bool OplogBufferCollection::pushAllNonBlocking(OperationContext* txn,
                                               Batch::const_iterator begin,
                                               Batch::const_iterator end) {
    size_t numDocs = std::distance(begin, end);
    Batch docsToInsert(numDocs);
    std::transform(begin, end, docsToInsert.begin(), addIdToDocument);

    auto status = _storageInterface->insertDocuments(txn, _nss, docsToInsert);
    if (!status.isOK()) {
        LOG(1) << "Pushing oplog entries to OplogBufferCollection failed with: " << status;
        return false;
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _count += numDocs;
    _size += std::accumulate(begin, end, 0U, [](const size_t& docSize, const Value& value) {
        return docSize + size_t(value.objsize());
    });
    return true;
}

void OplogBufferCollection::waitForSpace(OperationContext* txn, std::size_t size) {}

bool OplogBufferCollection::isEmpty() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _count == 0;
}

std::size_t OplogBufferCollection::getMaxSize() const {
    return 0;
}

std::size_t OplogBufferCollection::getSize() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _size;
}

std::size_t OplogBufferCollection::getCount() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _count;
}

void OplogBufferCollection::clear(OperationContext* txn) {
    _dropCollection(txn);
    _createCollection(txn);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _size = 0;
    _count = 0;
}

bool OplogBufferCollection::tryPop(OperationContext* txn, Value* value) {
    auto keyPattern = BSON("_id" << 1);
    auto scanDirection = StorageInterface::ScanDirection::kForward;
    auto result = _storageInterface->deleteOne(txn, _nss, keyPattern, scanDirection);
    if (!result.isOK()) {
        if (result != ErrorCodes::NoSuchKey) {
            LOG(1) << "Popping oplog entries from OplogBufferCollection failed with: "
                   << result.getStatus();
        }
        return false;
    }
    *value = extractEmbeddedOplogDocument(result.getValue()).getOwned();
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_count > 0);
    invariant(_size >= std::size_t(value->objsize()));
    _count--;
    _size -= value->objsize();
    return true;
}

OplogBuffer::Value OplogBufferCollection::blockingPop(OperationContext* txn) {
    return {};
}

bool OplogBufferCollection::blockingPeek(OperationContext* txn,
                                         Value* value,
                                         Seconds waitDuration) {
    return false;
}

bool OplogBufferCollection::peek(OperationContext* txn, Value* value) {
    return _peekOneSide(txn, value, true);
}

boost::optional<OplogBuffer::Value> OplogBufferCollection::lastObjectPushed(
    OperationContext* txn) const {
    Value value;
    bool res = _peekOneSide(txn, &value, false);
    if (!res) {
        return boost::none;
    }
    return value;
}

bool OplogBufferCollection::_peekOneSide(OperationContext* txn, Value* value, bool front) const {
    auto keyPattern = BSON("_id" << 1);
    auto scanDirection = front ? StorageInterface::ScanDirection::kForward
                               : StorageInterface::ScanDirection::kBackward;
    auto result = _storageInterface->findOne(txn, _nss, keyPattern, scanDirection);
    if (!result.isOK()) {
        LOG(1) << "Peeking oplog entries from OplogBufferCollection failed with: "
               << result.getStatus();
        return false;
    }
    *value = extractEmbeddedOplogDocument(result.getValue()).getOwned();
    return true;
}

void OplogBufferCollection::_createCollection(OperationContext* txn) {
    CollectionOptions options;
    options.temp = true;
    fassert(40154, _storageInterface->createCollection(txn, _nss, options));
}

void OplogBufferCollection::_dropCollection(OperationContext* txn) {
    fassert(40155, _storageInterface->dropCollection(txn, _nss));
}

}  // namespace repl
}  // namespace mongo
