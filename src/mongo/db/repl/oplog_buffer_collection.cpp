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

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {

const StringData kDefaultOplogCollectionNamespace = "local.temp_oplog_buffer"_sd;
const StringData kOplogEntryFieldName = "entry"_sd;
const StringData kIdIdxName = "_id_"_sd;

}  // namespace

NamespaceString OplogBufferCollection::getDefaultNamespace() {
    return NamespaceString(kDefaultOplogCollectionNamespace);
}

std::pair<BSONObj, Timestamp> OplogBufferCollection::addIdToDocument(const BSONObj& orig) {
    invariant(!orig.isEmpty());
    BSONObjBuilder bob;
    Timestamp ts = orig["ts"].timestamp();
    invariant(!ts.isNull());
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
    clear(txn);
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

void OplogBufferCollection::pushAllNonBlocking(OperationContext* txn,
                                               Batch::const_iterator begin,
                                               Batch::const_iterator end) {
    if (begin == end) {
        return;
    }
    size_t numDocs = std::distance(begin, end);
    Batch docsToInsert(numDocs);
    Timestamp ts;
    std::transform(begin, end, docsToInsert.begin(), [&ts](const Value& value) {
        auto pair = addIdToDocument(value);
        invariant(ts.isNull() || pair.second > ts);
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
    return _pop_inlock(txn, value);
}

bool OplogBufferCollection::waitForData(Seconds waitDuration) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (!_cvNoLongerEmpty.wait_for(
            lk, waitDuration.toSystemDuration(), [&]() { return _count != 0; })) {
        return false;
    }
    return _count != 0;
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

bool OplogBufferCollection::_pop_inlock(OperationContext* txn, Value* value) {
    // If there is a sentinel, and it was pushed right after the last BSONObj to be popped was
    // pushed, then we pop off a sentinel instead and decrease the count by 1.
    if (!_sentinels.empty() && (_lastPoppedTimestamp == _sentinels.front())) {
        _sentinels.pop();
        _count--;
        *value = BSONObj();
        return true;
    }

    if (!_peekOneSide_inlock(txn, value, true)) {
        return false;
    }

    _lastPoppedTimestamp = (*value)["ts"].timestamp();
    invariant(_count > 0);
    invariant(_size >= std::size_t(value->objsize()));
    _count--;
    _size -= value->objsize();
    return true;
}

bool OplogBufferCollection::_peekOneSide_inlock(OperationContext* txn,
                                                Value* value,
                                                bool front) const {
    invariant(_count > 0);

    // If there is a sentinel, and it was pushed right after the last BSONObj to be popped was
    // pushed, then we return an empty BSONObj for the sentinel.
    if (!_sentinels.empty() && (_lastPoppedTimestamp == _sentinels.front())) {
        *value = BSONObj();
        return true;
    }
    auto scanDirection = front ? StorageInterface::ScanDirection::kForward
                               : StorageInterface::ScanDirection::kBackward;
    BSONObj startKey;
    auto boundInclusion = BoundInclusion::kIncludeStartKeyOnly;

    // Previously popped documents are not actually removed from the collection. When peeking at the
    // front of the buffer, we use the last popped timestamp to skip ahead to the first document
    // that has not been popped.
    if (front && !_lastPoppedTimestamp.isNull()) {
        startKey = BSON("" << _lastPoppedTimestamp);
        boundInclusion = BoundInclusion::kIncludeEndKeyOnly;
    }

    const auto docs =
        fassertStatusOK(40163,
                        _storageInterface->findDocuments(
                            txn, _nss, kIdIdxName, scanDirection, startKey, boundInclusion, 1U));
    invariant(1U == docs.size());
    *value = extractEmbeddedOplogDocument(docs.front()).getOwned();
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
