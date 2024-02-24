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

#pragma once

#include <boost/optional/optional.hpp>
#include <cstddef>
#include <queue>
#include <tuple>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/queue.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

class StorageInterface;

/**
 * Oplog buffer backed by an optionally temporary collection. This collection is optionally created
 * in startup() and removed in shutdown(). The documents will be popped and peeked in timestamp
 * order.
 */
class OplogBufferCollection : public RandomAccessOplogBuffer {
public:
    /**
     * Structure used to configure an instance of OplogBufferCollection.
     */
    struct Options {
        // If equal to 0, the cache size will be set to 1.
        std::size_t peekCacheSize = 0;
        bool dropCollectionAtStartup = true;
        bool dropCollectionAtShutdown = true;
        bool useTemporaryCollection = true;
        Options() {}
    };

    /**
     * Returns default namespace for collection used to hold data in oplog buffer.
     */
    static NamespaceString getDefaultNamespace();

    /**
     * Returns the embedded document in the 'entry' field.
     */
    static BSONObj extractEmbeddedOplogDocument(const BSONObj& orig);


    /**
     * Creates and returns a document suitable for storing in the collection together with the
     * associated timestamp that determines the position of this document in the _id index.
     *
     * The '_id' field of the returned BSONObj will be:
     * {
     *     ts: 'ts' field of the provided document,
     * }
     *
     * The oplog entry itself will be stored in the 'entry' field of the returned BSONObj.
     */
    static std::tuple<BSONObj, Timestamp> addIdToDocument(const BSONObj& orig);

    explicit OplogBufferCollection(StorageInterface* storageInterface, Options options = Options());
    OplogBufferCollection(StorageInterface* storageInterface,
                          const NamespaceString& nss,
                          Options options = Options());

    /**
     * Returns the namespace string of the collection used by this oplog buffer.
     */
    NamespaceString getNamespace() const;

    /**
     * Returns the options used to configure this OplogBufferCollection
     */
    Options getOptions() const;

    void startup(OperationContext* opCtx) override;
    void shutdown(OperationContext* opCtx) override;

    // --- CAUTION: Push() and preload() are legal to be called only after startup() ---

    void push(OperationContext* opCtx,
              Batch::const_iterator begin,
              Batch::const_iterator end,
              boost::optional<std::size_t> bytes = boost::none) override;
    /**
     * Like push(), but allows the operations in the batch to be out of order with
     * respect to themselves and to the buffer. Legal to be called only before reading anything,
     * or immediately after a clear().
     */
    void preload(OperationContext* opCtx, Batch::const_iterator begin, Batch::const_iterator end);

    void waitForSpace(OperationContext* opCtx, std::size_t size) override;
    bool isEmpty() const override;
    std::size_t getMaxSize() const override;
    std::size_t getSize() const override;
    std::size_t getCount() const override;
    void clear(OperationContext* opCtx) override;
    bool tryPop(OperationContext* opCtx, Value* value) override;
    bool waitForDataFor(Milliseconds waitDuration, Interruptible* interruptible) override;
    bool waitForDataUntil(Date_t deadline, Interruptible* interruptible) override;
    bool peek(OperationContext* opCtx, Value* value) override;
    boost::optional<Value> lastObjectPushed(OperationContext* opCtx) const override;

    // ---- Random access API ----
    StatusWith<Value> findByTimestamp(OperationContext* opCtx, const Timestamp& ts) final;
    // Note: once you use seekToTimestamp, calling getSize() is no longer legal.
    Status seekToTimestamp(OperationContext* opCtx,
                           const Timestamp& ts,
                           SeekStrategy exact = SeekStrategy::kExact) final;

    // Only currently used by the TenantMigrationRecipientService, so not part of a parent API.
    Timestamp getLastPushedTimestamp() const;

    // ---- Testing API ----
    Timestamp getLastPoppedTimestamp_forTest() const;
    std::queue<BSONObj> getPeekCache_forTest() const;

private:
    /*
     * Creates an (optionally temporary) collection with the _nss namespace.
     */
    void _createCollection(OperationContext* opCtx);

    /*
     * Drops the collection with the _nss namespace.
     */
    void _dropCollection(OperationContext* opCtx);

    enum class PeekMode { kExtractEmbeddedDocument, kReturnUnmodifiedDocumentFromCollection };
    /**
     * Returns the oldest oplog entry in the buffer.
     * Assumes the buffer is not empty.
     */
    BSONObj _peek_inlock(OperationContext* opCtx, PeekMode peekMode);

    // Storage interface used to perform storage engine level functions on the collection.
    StorageInterface* _storageInterface;

    /**
     * Pops an entry off the buffer in a lock.
     */
    bool _pop_inlock(OperationContext* opCtx, Value* value);

    /**
     * Puts documents in collection without checking for order and without updating
     * _lastPushedTimestamp.
     */
    void _push(WithLock,
               OperationContext* opCtx,
               Batch::const_iterator begin,
               Batch::const_iterator end);
    /**
     * Returns the last document pushed onto the collection. This does not remove the `_id` field
     * of the document. If the collection is empty, this returns boost::none.
     */
    boost::optional<Value> _lastDocumentPushed_inlock(OperationContext* opCtx) const;

    /**
     * Updates '_lastPushedTimestamp' based on the last document in the collection.
     */
    void _updateLastPushedTimestampFromCollection(WithLock, OperationContext* opCtx);

    /**
     * Returns the document with the given timestamp, or ErrorCodes::NoSuchKey if not found.
     */
    StatusWith<BSONObj> _getDocumentWithTimestamp(OperationContext* opCtx, const Timestamp& ts);

    /**
     * Returns the key for the document with the given timestamp.
     */
    static BSONObj _keyForTimestamp(const Timestamp& ts);

    // The namespace for the oplog buffer collection.
    const NamespaceString _nss;

    // These are the options with which the oplog buffer was configured at construction time.
    const Options _options;

    // Allows functions to wait until the queue has data. This condition variable is used with
    // _mutex below.
    stdx::condition_variable _cvNoLongerEmpty;

    // Protects member data below and synchronizes it with the underlying collection.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("OplogBufferCollection::_mutex");

    // Number of documents in buffer.
    std::size_t _count = 0;

    // Size of documents in buffer.
    std::size_t _size = 0;

    Timestamp _lastPushedTimestamp;

    BSONObj _lastPoppedKey;

    // Used by _peek_inlock() to hold results of the read ahead query that will be used for pop/peek
    // results.
    std::queue<BSONObj> _peekCache;

    // Whether or not the size() method can be called.  This is set to false on seek, because
    // we do not know how much we skipped when seeking.
    bool _sizeIsValid = true;
};

}  // namespace repl
}  // namespace mongo
