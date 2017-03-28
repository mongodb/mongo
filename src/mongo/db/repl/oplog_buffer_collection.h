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

#pragma once

#include <queue>
#include <tuple>

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/queue.h"

namespace mongo {
namespace repl {

class StorageInterface;

/**
 * Oplog buffer backed by a temporary collection. This collection is created in startup() and
 * removed in shutdown(). The documents will be popped and peeked in timestamp order.
 */
class OplogBufferCollection : public OplogBuffer {
public:
    /**
     * Structure used to configure an instance of OplogBufferCollection.
     */
    struct Options {
        // If equal to 0, the cache size will be set to 1.
        std::size_t peekCacheSize = 0;
        bool dropCollectionAtStartup = true;
        bool dropCollectionAtShutdown = true;
        Options() {}
    };

    /**
     * Returns default namespace for temporary collection used to hold data in oplog buffer.
     */
    static NamespaceString getDefaultNamespace();

    /**
     * Returns the embedded document in the 'entry' field.
     */
    static BSONObj extractEmbeddedOplogDocument(const BSONObj& orig);


    /**
     * Creates and returns a document suitable for storing in the collection together with the
     * associated timestamp and sentinel count that determines the position of this document in the
     * _id index.
     *
     * If 'orig' is a valid oplog entry, the '_id' field of the returned BSONObj will be:
     * {
     *     ts: 'ts' field of the provided document,
     *     s: 0
     * }
     * The timestamp returned will be equal to as the 'ts' field in the BSONObj.
     * Assumes there is a 'ts' field in the original document.
     *
     * If 'orig' is an empty document (ie. we're inserting a sentinel value), the '_id' field will
     * be generated based on the timestamp of the last document processed and the total number of
     * sentinels with the same timestamp (including the document about to be inserted. For example,
     * the first sentinel to be inserted after a valid oplog entry will have the following '_id'
     * field:
     * {
     *     ts: 'ts' field of the last inserted valid oplog entry,
     *     s: 1
     * }
     * The sentinel counter will be reset to 0 on inserting the next valid oplog entry.
     */
    static std::tuple<BSONObj, Timestamp, std::size_t> addIdToDocument(
        const BSONObj& orig, const Timestamp& lastTimestamp, std::size_t sentinelCount);

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
    void pushEvenIfFull(OperationContext* opCtx, const Value& value) override;
    void push(OperationContext* opCtx, const Value& value) override;
    /**
     * Pushing documents with 'pushAllNonBlocking' will not handle sentinel documents properly. If
     * pushing sentinel documents is required, use 'push' or 'pushEvenIfFull'.
     */
    void pushAllNonBlocking(OperationContext* opCtx,
                            Batch::const_iterator begin,
                            Batch::const_iterator end) override;
    void waitForSpace(OperationContext* opCtx, std::size_t size) override;
    bool isEmpty() const override;
    std::size_t getMaxSize() const override;
    std::size_t getSize() const override;
    std::size_t getCount() const override;
    void clear(OperationContext* opCtx) override;
    bool tryPop(OperationContext* opCtx, Value* value) override;
    bool waitForData(Seconds waitDuration) override;
    bool peek(OperationContext* opCtx, Value* value) override;
    boost::optional<Value> lastObjectPushed(OperationContext* opCtx) const override;

    // ---- Testing API ----
    std::size_t getSentinelCount_forTest() const;
    Timestamp getLastPushedTimestamp_forTest() const;
    Timestamp getLastPoppedTimestamp_forTest() const;
    std::queue<BSONObj> getPeekCache_forTest() const;

private:
    /*
     * Creates a temporary collection with the _nss namespace.
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
     * Returns the last document pushed onto the collection. This does not remove the `_id` field
     * of the document. If the collection is empty, this returns boost::none.
     */
    boost::optional<Value> _lastDocumentPushed_inlock(OperationContext* opCtx) const;

    // The namespace for the oplog buffer collection.
    const NamespaceString _nss;

    // These are the options with which the oplog buffer was configured at construction time.
    const Options _options;

    // Allows functions to wait until the queue has data. This condition variable is used with
    // _mutex below.
    stdx::condition_variable _cvNoLongerEmpty;

    // Protects member data below and synchronizes it with the underlying collection.
    mutable stdx::mutex _mutex;

    // Number of documents in buffer.
    std::size_t _count = 0;

    // Size of documents in buffer.
    std::size_t _size = 0;

    // Number of sentinel values inserted so far with the same timestamp as '_lastPoppedKey'.
    std::size_t _sentinelCount = 0;

    Timestamp _lastPushedTimestamp;

    BSONObj _lastPoppedKey;

    // Used by _peek_inlock() to hold results of the read ahead query that will be used for pop/peek
    // results.
    std::queue<BSONObj> _peekCache;
};

}  // namespace repl
}  // namespace mongo
