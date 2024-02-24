/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <iterator>
#include <mutex>
#include <numeric>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/single_write_result_gen.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

namespace {

const StringData kOplogEntryFieldName = "entry"_sd;
const StringData kIdFieldName = "_id"_sd;
const StringData kTimestampFieldName = "ts"_sd;
const StringData kIdIdxName = "_id_"_sd;

const Timestamp kInvalidLastPushedTimestamp(0, 1);
}  // namespace

NamespaceString OplogBufferCollection::getDefaultNamespace() {
    return NamespaceString::kDefaultOplogCollectionNamespace;
}

std::tuple<BSONObj, Timestamp> OplogBufferCollection::addIdToDocument(const BSONObj& orig) {
    invariant(!orig.isEmpty());
    const auto ts = orig[kTimestampFieldName].timestamp();
    invariant(!ts.isNull());
    auto doc = BSON(_keyForTimestamp(ts).firstElement() << kOplogEntryFieldName << orig);
    return std::make_tuple(doc, ts);
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

    // If the collection doesn't already exist, create it.
    _createCollection(opCtx);

    stdx::lock_guard<Latch> lk(_mutex);
    // If we are starting from an existing collection, we must populate the in memory state of the
    // buffer.
    _size = uassertStatusOK(_storageInterface->getCollectionSize(opCtx, _nss));
    _sizeIsValid = true;

    _count = uassertStatusOK(_storageInterface->getCollectionCount(opCtx, _nss));

    // We always start from the beginning, with _lastPoppedKey being empty. This is safe because
    // it is always safe to replay old oplog entries in order. We explicitly reset all fields
    // since nothing prevents reusing an OplogBufferCollection, and the underlying collection may
    // have changed since the last time we used this OplogBufferCollection.
    _lastPoppedKey = {};
    _peekCache = std::queue<BSONObj>();

    _updateLastPushedTimestampFromCollection(lk, opCtx);
}

void OplogBufferCollection::_updateLastPushedTimestampFromCollection(WithLock,
                                                                     OperationContext* opCtx) {
    auto lastPushedObj = _lastDocumentPushed_inlock(opCtx);
    if (lastPushedObj) {
        auto lastPushedId = lastPushedObj->getObjectField(kIdFieldName);
        fassert(
            40405,
            bsonExtractTimestampField(lastPushedId, kTimestampFieldName, &_lastPushedTimestamp));
    } else {
        _lastPushedTimestamp = {};
    }
}

void OplogBufferCollection::shutdown(OperationContext* opCtx) {
    if (_options.dropCollectionAtShutdown) {
        stdx::lock_guard<Latch> lk(_mutex);
        _dropCollection(opCtx);
        _size = 0;
        _count = 0;
        _lastPushedTimestamp = {};
        _lastPoppedKey = {};
        _peekCache = std::queue<BSONObj>();
    }
}

void OplogBufferCollection::push(OperationContext* opCtx,
                                 Batch::const_iterator begin,
                                 Batch::const_iterator end,
                                 boost::optional<std::size_t> bytes) {
    if (begin == end) {
        return;
    }
    stdx::lock_guard<Latch> lk(_mutex);
    // Make sure timestamp order is correct.
    auto ts = _lastPushedTimestamp;
    std::for_each(begin, end, [&ts](const Value& value) {
        auto previousTimestamp = ts;
        ts = value[kTimestampFieldName].timestamp();
        invariant(!ts.isNull());
        invariant(ts > previousTimestamp,
                  str::stream() << "ts: " << ts.toString()
                                << ", previous: " << previousTimestamp.toString());
    });

    _push(lk, opCtx, begin, end);
    _lastPushedTimestamp = ts;
}

void OplogBufferCollection::preload(OperationContext* opCtx,
                                    Batch::const_iterator begin,
                                    Batch::const_iterator end) {
    if (begin == end) {
        return;
    }

    ScopeGuard failToPreloadGuard([this] {
        stdx::unique_lock lk(_mutex);
        _lastPushedTimestamp = kInvalidLastPushedTimestamp;
    });

    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_lastPoppedKey.isEmpty());
    _push(lk, opCtx, begin, end);
    _updateLastPushedTimestampFromCollection(lk, opCtx);

    failToPreloadGuard.dismiss();
}

void OplogBufferCollection::_push(WithLock,
                                  OperationContext* opCtx,
                                  Batch::const_iterator begin,
                                  Batch::const_iterator end) {
    size_t numDocs = std::distance(begin, end);
    std::vector<BSONObj> docsToInsert(numDocs);
    std::transform(begin, end, docsToInsert.begin(), [](const Value& value) {
        auto [doc, ts] = addIdToDocument(value);
        invariant(!value.isEmpty());
        return doc;
    });

    // Disabling internal document validation because the oplog buffer document inserts
    // can violate max data size limit (which is BSONObjMaxUserSize 16MB) check. Since, the max
    // user document size is 16MB, the oplog generated for those writes can exceed 16MB
    // (16MB user data  + additional bytes for oplog fields like ’’op”, “ns”, “ui”).
    DisableDocumentValidation documentValidationDisabler(
        opCtx,
        DocumentValidationSettings::kDisableSchemaValidation |
            DocumentValidationSettings::kDisableInternalValidation);

    write_ops::InsertCommandRequest insertOp(_nss);
    insertOp.setDocuments(std::move(docsToInsert));
    insertOp.setWriteCommandRequestBase([] {
        write_ops::WriteCommandRequestBase wcb;
        wcb.setOrdered(true);
        return wcb;
    }());

    auto writeResult = write_ops_exec::performInserts(opCtx, insertOp);
    invariant(!writeResult.results.empty());
    // Since the writes are ordered, it's ok to check just the last writeOp result.
    uassertStatusOK(writeResult.results.back());

    _count += numDocs;
    if (_sizeIsValid) {
        _size += std::accumulate(begin, end, 0U, [](const size_t& docSize, const Value& value) {
            return docSize + size_t(value.objsize());
        });
    }
    _cvNoLongerEmpty.notify_all();
}

void OplogBufferCollection::waitForSpace(OperationContext* opCtx, std::size_t size) {}

bool OplogBufferCollection::isEmpty() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _count == 0;
}

std::size_t OplogBufferCollection::getMaxSize() const {
    return 0;
}

std::size_t OplogBufferCollection::getSize() const {
    stdx::lock_guard<Latch> lk(_mutex);
    uassert(4940100, "getSize() called on OplogBufferCollection after seek", _sizeIsValid);
    return _size;
}

std::size_t OplogBufferCollection::getCount() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _count;
}

void OplogBufferCollection::clear(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lk(_mutex);
    // We acquire the appropriate locks for the temporary oplog buffer collection here,
    // so that we perform the drop and create under the same locks.
    AutoGetCollection autoColl(opCtx, NamespaceString::kDefaultOplogCollectionNamespace, MODE_X);
    _dropCollection(opCtx);
    _createCollection(opCtx);
    _size = 0;
    _sizeIsValid = true;
    _count = 0;
    _lastPushedTimestamp = {};
    _lastPoppedKey = {};
    _peekCache = std::queue<BSONObj>();
}

bool OplogBufferCollection::tryPop(OperationContext* opCtx, Value* value) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_count == 0) {
        return false;
    }
    return _pop_inlock(opCtx, value);
}

bool OplogBufferCollection::waitForDataFor(Milliseconds waitDuration,
                                           Interruptible* interruptible) {
    stdx::unique_lock<Latch> lk(_mutex);
    if (!interruptible->waitForConditionOrInterruptFor(
            _cvNoLongerEmpty, lk, waitDuration, [&]() { return _count != 0; })) {
        return false;
    }
    return _count != 0;
}

bool OplogBufferCollection::waitForDataUntil(Date_t deadline, Interruptible* interruptible) {
    stdx::unique_lock<Latch> lk(_mutex);
    if (!interruptible->waitForConditionOrInterruptUntil(
            _cvNoLongerEmpty, lk, deadline, [&]() { return _count != 0; })) {
        return false;
    }
    return _count != 0;
}

bool OplogBufferCollection::peek(OperationContext* opCtx, Value* value) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_count == 0) {
        return false;
    }
    *value = _peek_inlock(opCtx, PeekMode::kExtractEmbeddedDocument);
    return true;
}

boost::optional<OplogBuffer::Value> OplogBufferCollection::lastObjectPushed(
    OperationContext* opCtx) const {
    stdx::lock_guard<Latch> lk(_mutex);
    auto lastDocumentPushed = _lastDocumentPushed_inlock(opCtx);
    if (lastDocumentPushed) {
        BSONObj entryObj = extractEmbeddedOplogDocument(*lastDocumentPushed);
        entryObj.shareOwnershipWith(*lastDocumentPushed);
        return entryObj;
    }
    return boost::none;
}

/* static */
BSONObj OplogBufferCollection::_keyForTimestamp(const Timestamp& ts) {
    return BSON(kIdFieldName << BSON(kTimestampFieldName << ts));
}

StatusWith<BSONObj> OplogBufferCollection::_getDocumentWithTimestamp(OperationContext* opCtx,
                                                                     const Timestamp& ts) {
    return _storageInterface->findById(opCtx, _nss, _keyForTimestamp(ts).firstElement());
}

StatusWith<OplogBuffer::Value> OplogBufferCollection::findByTimestamp(OperationContext* opCtx,
                                                                      const Timestamp& ts) {
    auto docWithStatus = _getDocumentWithTimestamp(opCtx, ts);
    if (!docWithStatus.isOK()) {
        return docWithStatus.getStatus();
    }
    return extractEmbeddedOplogDocument(docWithStatus.getValue()).getOwned();
}

Status OplogBufferCollection::seekToTimestamp(OperationContext* opCtx,
                                              const Timestamp& ts,
                                              SeekStrategy exact) {
    stdx::lock_guard<Latch> lk(_mutex);
    BSONObj docWithTimestamp;
    auto docWithStatus = _getDocumentWithTimestamp(opCtx, ts);
    if (docWithStatus.isOK()) {
        docWithTimestamp = std::move(docWithStatus.getValue());
    } else if (exact == SeekStrategy::kExact) {
        return docWithStatus.getStatus();
    }
    _peekCache = std::queue<BSONObj>();
    auto key = _keyForTimestamp(ts);
    if (docWithTimestamp.isEmpty()) {
        // The document with the requested timestamp was not found.  Set _lastPoppedKey to
        // the key for that document, so next time we pop we will read the next document after
        // the requested timestamp.
        _lastPoppedKey = key;
    } else {
        // The document with the requested timestamp was found.  _lastPoppedKey will be set to that
        // document's timestamp once the document is popped from the peek cache in _pop_inlock().
        _lastPoppedKey = {};
        _peekCache.push(docWithTimestamp);
    }
    // Unfortunately StorageInterface and InternalPlanner don't support count-by-index, so we're
    // stuck with DBDirectClient.
    DBDirectClient client(opCtx);
    auto query = BSON(kIdFieldName << BSON("$gte" << key[kIdFieldName]));
    _count = client.count(_nss, query);

    // We have no way of accurately determining the size remaining after the seek
    _sizeIsValid = false;
    return Status::OK();
}

boost::optional<OplogBuffer::Value> OplogBufferCollection::_lastDocumentPushed_inlock(
    OperationContext* opCtx) const {
    if (_count == 0) {
        return boost::none;
    }
    const auto docs =
        uassertStatusOK(_storageInterface->findDocuments(opCtx,
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
    if (_sizeIsValid) {
        invariant(_size >= std::size_t(value->objsize()));
        _size -= value->objsize();
    }
    _count--;
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
        const auto docs = uassertStatusOK(
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
    options.temp = _options.useTemporaryCollection;
    // This oplog-like collection will benefit from clustering by _id to reduce storage engine
    // overhead and improve _id query efficiency.
    options.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();

    auto status = _storageInterface->createCollection(opCtx, _nss, options);
    if (status.code() == ErrorCodes::NamespaceExists)
        return;
    uassertStatusOK(status);
}

void OplogBufferCollection::_dropCollection(OperationContext* opCtx) {
    uassertStatusOK(_storageInterface->dropCollection(opCtx, _nss));
}

Timestamp OplogBufferCollection::getLastPushedTimestamp() const {
    stdx::lock_guard<Latch> lk(_mutex);
    uassert(8359601,
            "preload() might have failed. So clear() should be called before reading "
            "'lastPushedTimestamp'",
            _lastPushedTimestamp != kInvalidLastPushedTimestamp);
    return _lastPushedTimestamp;
}

Timestamp OplogBufferCollection::getLastPoppedTimestamp_forTest() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _lastPoppedKey.isEmpty() ? Timestamp()
                                    : _lastPoppedKey[""].Obj()[kTimestampFieldName].timestamp();
}

std::queue<BSONObj> OplogBufferCollection::getPeekCache_forTest() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _peekCache;
}

}  // namespace repl
}  // namespace mongo
