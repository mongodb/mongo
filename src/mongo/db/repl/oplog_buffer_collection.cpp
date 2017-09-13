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
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {

const StringData kDefaultOplogCollectionNamespace = "local.temp_oplog_buffer"_sd;
const StringData kOplogEntryFieldName = "entry"_sd;
const StringData kIdFieldName = "_id"_sd;
const StringData kTimestampFieldName = "ts"_sd;
const StringData kSentinelFieldName = "s"_sd;
const StringData kIdIdxName = "_id_"_sd;

}  // namespace

NamespaceString OplogBufferCollection::getDefaultNamespace() {
    return NamespaceString(kDefaultOplogCollectionNamespace);
}

std::tuple<BSONObj, Timestamp, std::size_t> OplogBufferCollection::addIdToDocument(
    const BSONObj& orig, const Timestamp& lastTimestamp, std::size_t sentinelCount) {
    if (orig.isEmpty()) {
        return std::make_tuple(
            BSON(kIdFieldName << BSON(
                     kTimestampFieldName << lastTimestamp << kSentinelFieldName
                                         << static_cast<long long>(sentinelCount + 1))),
            lastTimestamp,
            sentinelCount + 1);
    }
    const auto ts = orig[kTimestampFieldName].timestamp();
    invariant(!ts.isNull());
    auto doc = BSON(kIdFieldName << BSON(kTimestampFieldName << ts << kSentinelFieldName << 0)
                                 << kOplogEntryFieldName
                                 << orig);
    return std::make_tuple(doc, ts, 0);
}

BSONObj OplogBufferCollection::extractEmbeddedOplogDocument(const BSONObj& orig) {
    return orig.getObjectField(kOplogEntryFieldName);
}


OplogBufferCollection::OplogBufferCollection(StorageInterface* storageInterface, Options options)
    : OplogBufferCollection(storageInterface, getDefaultNamespace(), std::move(options)) {}

OplogBufferCollection::OplogBufferCollection(StorageInterface* storageInterface,
                                             const NamespaceString& nss,
                                             Options options)
    : _storageInterface(storageInterface), _nss(nss), _options(std::move(options)) {}

NamespaceString OplogBufferCollection::getNamespace() const {
    return _nss;
}

OplogBufferCollection::Options OplogBufferCollection::getOptions() const {
    return _options;
}

void OplogBufferCollection::startup(OperationContext* opCtx) {
    if (_options.dropCollectionAtStartup) {
        clear(opCtx);
        return;
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    // If we are starting from an existing collection, we must populate the in memory state of the
    // buffer.
    auto sizeResult = _storageInterface->getCollectionSize(opCtx, _nss);
    fassertStatusOK(40403, sizeResult);
    _size = sizeResult.getValue();

    auto countResult = _storageInterface->getCollectionCount(opCtx, _nss);
    fassertStatusOK(40404, countResult);
    _count = countResult.getValue();

    // We always start from the beginning, with _lastPoppedKey being empty. This is safe because
    // it is always safe to replay old oplog entries in order. We explicitly reset all fields
    // since nothing prevents reusing an OplogBufferCollection, and the underlying collection may
    // have changed since the last time we used this OplogBufferCollection.
    _lastPoppedKey = {};
    _peekCache = std::queue<BSONObj>();

    if (_count == 0) {
        _sentinelCount = 0;
        _lastPushedTimestamp = {};
        return;
    }

    auto lastPushedObj = _lastDocumentPushed_inlock(opCtx);
    if (lastPushedObj) {
        auto lastPushedId = lastPushedObj->getObjectField(kIdFieldName);
        fassertStatusOK(
            40405,
            bsonExtractTimestampField(lastPushedId, kTimestampFieldName, &_lastPushedTimestamp));
        long long countAtTimestamp = 0;
        fassertStatusOK(
            40406, bsonExtractIntegerField(lastPushedId, kSentinelFieldName, &countAtTimestamp));
        _sentinelCount = countAtTimestamp--;
    } else {
        _lastPushedTimestamp = {};
        _sentinelCount = 0;
    }
}

void OplogBufferCollection::shutdown(OperationContext* opCtx) {
    if (_options.dropCollectionAtShutdown) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _dropCollection(opCtx);
        _size = 0;
        _count = 0;
        _sentinelCount = 0;
        _lastPushedTimestamp = {};
        _lastPoppedKey = {};
        _peekCache = std::queue<BSONObj>();
    }
}

void OplogBufferCollection::pushEvenIfFull(OperationContext* opCtx, const Value& value) {
    Batch valueBatch = {value};
    pushAllNonBlocking(opCtx, valueBatch.begin(), valueBatch.end());
}

void OplogBufferCollection::push(OperationContext* opCtx, const Value& value) {
    pushEvenIfFull(opCtx, value);
}

void OplogBufferCollection::pushAllNonBlocking(OperationContext* opCtx,
                                               Batch::const_iterator begin,
                                               Batch::const_iterator end) {
    if (begin == end) {
        return;
    }
    size_t numDocs = std::distance(begin, end);
    std::vector<InsertStatement> docsToInsert(numDocs);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto ts = _lastPushedTimestamp;
    auto sentinelCount = _sentinelCount;
    std::transform(begin, end, docsToInsert.begin(), [&sentinelCount, &ts](const Value& value) {
        BSONObj doc;
        auto previousTimestamp = ts;
        std::tie(doc, ts, sentinelCount) = addIdToDocument(value, ts, sentinelCount);
        invariant(value.isEmpty() ? ts == previousTimestamp : ts > previousTimestamp);
        return InsertStatement(doc);
    });

    auto status = _storageInterface->insertDocuments(opCtx, _nss, docsToInsert);
    fassertStatusOK(40161, status);

    _lastPushedTimestamp = ts;
    _sentinelCount = sentinelCount;
    _count += numDocs;
    _size += std::accumulate(begin, end, 0U, [](const size_t& docSize, const Value& value) {
        return docSize + size_t(value.objsize());
    });
    _cvNoLongerEmpty.notify_all();
}

void OplogBufferCollection::waitForSpace(OperationContext* opCtx, std::size_t size) {}

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

void OplogBufferCollection::clear(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _dropCollection(opCtx);
    _createCollection(opCtx);
    _size = 0;
    _count = 0;
    _sentinelCount = 0;
    _lastPushedTimestamp = {};
    _lastPoppedKey = {};
    _peekCache = std::queue<BSONObj>();
}

bool OplogBufferCollection::tryPop(OperationContext* opCtx, Value* value) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_count == 0) {
        return false;
    }
    return _pop_inlock(opCtx, value);
}

bool OplogBufferCollection::waitForData(Seconds waitDuration) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (!_cvNoLongerEmpty.wait_for(
            lk, waitDuration.toSystemDuration(), [&]() { return _count != 0; })) {
        return false;
    }
    return _count != 0;
}

bool OplogBufferCollection::peek(OperationContext* opCtx, Value* value) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_count == 0) {
        return false;
    }
    *value = _peek_inlock(opCtx, PeekMode::kExtractEmbeddedDocument);
    return true;
}

boost::optional<OplogBuffer::Value> OplogBufferCollection::lastObjectPushed(
    OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto lastDocumentPushed = _lastDocumentPushed_inlock(opCtx);
    if (lastDocumentPushed) {
        BSONObj entryObj = extractEmbeddedOplogDocument(*lastDocumentPushed);
        entryObj.shareOwnershipWith(*lastDocumentPushed);
        return entryObj;
    }
    return boost::none;
}

boost::optional<OplogBuffer::Value> OplogBufferCollection::_lastDocumentPushed_inlock(
    OperationContext* opCtx) const {
    if (_count == 0) {
        return boost::none;
    }
    const auto docs =
        fassertStatusOK(40348,
                        _storageInterface->findDocuments(opCtx,
                                                         _nss,
                                                         kIdIdxName,
                                                         StorageInterface::ScanDirection::kBackward,
                                                         {},
                                                         BoundInclusion::kIncludeStartKeyOnly,
                                                         1U));
    invariant(1U == docs.size());
    return docs.front();
}

bool OplogBufferCollection::_pop_inlock(OperationContext* opCtx, Value* value) {
    BSONObj docFromCollection =
        _peek_inlock(opCtx, PeekMode::kReturnUnmodifiedDocumentFromCollection);
    _lastPoppedKey = docFromCollection[kIdFieldName].wrap("");
    *value = extractEmbeddedOplogDocument(docFromCollection).getOwned();

    invariant(!_peekCache.empty());
    invariant(!SimpleBSONObjComparator::kInstance.compare(docFromCollection, _peekCache.front()));
    _peekCache.pop();

    invariant(_count > 0);
    invariant(_size >= std::size_t(value->objsize()));
    _count--;
    _size -= value->objsize();
    return true;
}

BSONObj OplogBufferCollection::_peek_inlock(OperationContext* opCtx, PeekMode peekMode) {
    invariant(_count > 0);

    BSONObj startKey;
    auto boundInclusion = BoundInclusion::kIncludeStartKeyOnly;

    // Previously popped documents are not actually removed from the collection. We use the last
    // popped key to skip ahead to the first document that has not been popped.
    if (!_lastPoppedKey.isEmpty()) {
        startKey = _lastPoppedKey;
        boundInclusion = BoundInclusion::kIncludeEndKeyOnly;
    }

    bool isPeekCacheEnabled = _options.peekCacheSize > 0;
    // Check read ahead cache and read additional documents into cache if necessary - only valid
    // when size of read ahead cache is greater than zero in the options.
    if (_peekCache.empty()) {
        std::size_t limit = isPeekCacheEnabled ? _options.peekCacheSize : 1U;
        const auto docs = fassertStatusOK(
            40163,
            _storageInterface->findDocuments(opCtx,
                                             _nss,
                                             kIdIdxName,
                                             StorageInterface::ScanDirection::kForward,
                                             startKey,
                                             boundInclusion,
                                             limit));
        invariant(!docs.empty());
        for (const auto& doc : docs) {
            _peekCache.push(doc);
        }
    }
    auto&& doc = _peekCache.front();

    switch (peekMode) {
        case PeekMode::kExtractEmbeddedDocument:
            return extractEmbeddedOplogDocument(doc).getOwned();
            break;
        case PeekMode::kReturnUnmodifiedDocumentFromCollection:
            invariant(doc.isOwned());
            return doc;
            break;
    }

    MONGO_UNREACHABLE;
}

void OplogBufferCollection::_createCollection(OperationContext* opCtx) {
    CollectionOptions options;
    options.temp = true;
    fassert(40154, _storageInterface->createCollection(opCtx, _nss, options));
}

void OplogBufferCollection::_dropCollection(OperationContext* opCtx) {
    fassert(40155, _storageInterface->dropCollection(opCtx, _nss));
}

std::size_t OplogBufferCollection::getSentinelCount_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _sentinelCount;
}

Timestamp OplogBufferCollection::getLastPushedTimestamp_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _lastPushedTimestamp;
}

Timestamp OplogBufferCollection::getLastPoppedTimestamp_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _lastPoppedKey.isEmpty() ? Timestamp()
                                    : _lastPoppedKey[""].Obj()[kTimestampFieldName].timestamp();
}

std::queue<BSONObj> OplogBufferCollection::getPeekCache_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _peekCache;
}

}  // namespace repl
}  // namespace mongo
