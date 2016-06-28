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
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {

const char kDefaultOplogCollectionNamespace[] = "local.temp_oplog_buffer";
const char kOplogEntryFieldName[] = "entry";
const BSONObj kIdObj = BSON("_id" << 1);

}  // namespace

NamespaceString OplogBufferCollection::getDefaultNamespace() {
    return NamespaceString(kDefaultOplogCollectionNamespace);
}

std::pair<BSONObj, Timestamp> OplogBufferCollection::addIdToDocument(const BSONObj& orig) {
    invariant(!orig.isEmpty());
    BSONObjBuilder bob;
    Timestamp ts = orig["ts"].timestamp();
    bob.append("_id", ts);
    bob.append(kOplogEntryFieldName, orig);
    return std::pair<BSONObj, Timestamp>{bob.obj(), ts};
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _dropCollection(txn);
    _size = 0;
    _count = 0;
}

void OplogBufferCollection::pushEvenIfFull(OperationContext* txn, const Value& value) {
    // This oplog entry is a sentinel
    if (value.isEmpty()) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _sentinels.push(_lastPushedTimestamp);
        _count++;
        _cvNoLongerEmpty.notify_all();
        return;
    }
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
    Timestamp ts;
    std::transform(begin, end, docsToInsert.begin(), [&ts](const Value& value) {
        auto pair = addIdToDocument(value);
        ts = pair.second;
        return pair.first;
    });

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto status = _storageInterface->insertDocuments(txn, _nss, docsToInsert);
    fassertStatusOK(40161, status);

    _lastPushedTimestamp = ts;
    _count += numDocs;
    _size += std::accumulate(begin, end, 0U, [](const size_t& docSize, const Value& value) {
        return docSize + size_t(value.objsize());
    });
    _cvNoLongerEmpty.notify_all();
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
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _dropCollection(txn);
    _createCollection(txn);
    _size = 0;
    _count = 0;
    std::queue<Timestamp>().swap(_sentinels);
}

bool OplogBufferCollection::tryPop(OperationContext* txn, Value* value) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_count == 0) {
        return false;
    }
    return _doPop_inlock(txn, value);
}

OplogBuffer::Value OplogBufferCollection::blockingPop(OperationContext* txn) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _cvNoLongerEmpty.wait(lk, [&]() { return _count != 0; });
    BSONObj value;
    _doPop_inlock(txn, &value);
    return value;
}

bool OplogBufferCollection::blockingPeek(OperationContext* txn,
                                         Value* value,
                                         Seconds waitDuration) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (!_cvNoLongerEmpty.wait_for(
            lk, waitDuration.toSystemDuration(), [&]() { return _count != 0; })) {
        return false;
    }
    return _peekOneSide_inlock(txn, value, true);
}

bool OplogBufferCollection::peek(OperationContext* txn, Value* value) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_count == 0) {
        return false;
    }
    return _peekOneSide_inlock(txn, value, true);
}

boost::optional<OplogBuffer::Value> OplogBufferCollection::lastObjectPushed(
    OperationContext* txn) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_count == 0) {
        return boost::none;
    }
    Value value;
    bool res = _peekOneSide_inlock(txn, &value, false);
    if (!res) {
        return boost::none;
    }
    return value;
}

bool OplogBufferCollection::_doPop_inlock(OperationContext* txn, Value* value) {
    // If there is a sentinel, and it was pushed right after the last BSONObj to be popped was
    // pushed, then we pop off a sentinel instead and decrease the count by 1.
    if (!_sentinels.empty() && (_lastPoppedTimestamp == _sentinels.front())) {
        _sentinels.pop();
        _count--;
        *value = BSONObj();
        return true;
    }
    auto scanDirection = StorageInterface::ScanDirection::kForward;
    auto result = _storageInterface->deleteOne(txn, _nss, kIdObj, scanDirection);
    if (!result.isOK()) {
        if (result != ErrorCodes::CollectionIsEmpty) {
            fassert(40162, result.getStatus());
        }
        return false;
    }
    _lastPoppedTimestamp = result.getValue()["_id"].timestamp();
    *value = extractEmbeddedOplogDocument(result.getValue()).getOwned();
    invariant(_count > 0);
    invariant(_size >= std::size_t(value->objsize()));
    _count--;
    _size -= value->objsize();
    return true;
}

bool OplogBufferCollection::_peekOneSide_inlock(OperationContext* txn,
                                                Value* value,
                                                bool front) const {
    // If there is a sentinel, and it was pushed right after the last BSONObj to be popped was
    // pushed, then we return an empty BSONObj for the sentinel.
    if (!_sentinels.empty() && (_lastPoppedTimestamp == _sentinels.front())) {
        *value = BSONObj();
        return true;
    }
    auto scanDirection = front ? StorageInterface::ScanDirection::kForward
                               : StorageInterface::ScanDirection::kBackward;
    auto result = _storageInterface->findOne(txn, _nss, kIdObj, scanDirection);
    if (!result.isOK()) {
        if (result != ErrorCodes::CollectionIsEmpty) {
            fassert(40163, result.getStatus());
        }
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

std::queue<Timestamp> OplogBufferCollection::getSentinels_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _sentinels;
}

}  // namespace repl
}  // namespace mongo
