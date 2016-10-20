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

    explicit OplogBufferCollection(StorageInterface* storageInterface);
    OplogBufferCollection(StorageInterface* storageInterface, const NamespaceString& nss);

    /**
     * Returns the namespace string of the collection used by this oplog buffer.
     */
    NamespaceString getNamespace() const;

    void startup(OperationContext* txn) override;
    void shutdown(OperationContext* txn) override;
    void pushEvenIfFull(OperationContext* txn, const Value& value) override;
    void push(OperationContext* txn, const Value& value) override;
    /**
     * Pushing documents with 'pushAllNonBlocking' will not handle sentinel documents properly. If
     * pushing sentinel documents is required, use 'push' or 'pushEvenIfFull'.
     */
    void pushAllNonBlocking(OperationContext* txn,
                            Batch::const_iterator begin,
                            Batch::const_iterator end) override;
    void waitForSpace(OperationContext* txn, std::size_t size) override;
    bool isEmpty() const override;
    std::size_t getMaxSize() const override;
    std::size_t getSize() const override;
    std::size_t getCount() const override;
    void clear(OperationContext* txn) override;
    bool tryPop(OperationContext* txn, Value* value) override;
    bool waitForData(Seconds waitDuration) override;
    bool peek(OperationContext* txn, Value* value) override;
    boost::optional<Value> lastObjectPushed(OperationContext* txn) const override;

    // ---- Testing API ----
    std::size_t getSentinelCount_forTest() const;
    Timestamp getLastPushedTimestamp_forTest() const;
    Timestamp getLastPoppedTimestamp_forTest() const;

private:
    /*
     * Creates a temporary collection with the _nss namespace.
     */
    void _createCollection(OperationContext* txn);

    /*
     * Drops the collection with the _nss namespace.
     */
    void _dropCollection(OperationContext* txn);

    enum class PeekMode { kExtractEmbeddedDocument, kReturnUnmodifiedDocumentFromCollection };
    /**
     * Returns the last oplog entry on the given side of the buffer. If front is true it will
     * return the oldest entry, otherwise it will return the newest one. If the buffer is empty
     * or peeking fails this returns false.
     */
    bool _peekOneSide_inlock(OperationContext* txn,
                             Value* value,
                             bool front,
                             PeekMode peekMode) const;

    // Storage interface used to perform storage engine level functions on the collection.
    StorageInterface* _storageInterface;

    /**
     * Pops an entry off the buffer in a lock.
     */
    bool _pop_inlock(OperationContext* txn, Value* value);

    // The namespace for the oplog buffer collection.
    const NamespaceString _nss;

    // Allows functions to wait until the queue has data. This condition variable is used with
    // _mutex below.
    stdx::condition_variable _cvNoLongerEmpty;

    // Protects member data below and synchronizes it with the underlying collection.
    mutable stdx::mutex _mutex;

    // Number of documents in buffer.
    std::size_t _count;

    // Size of documents in buffer.
    std::size_t _size;

    // Number of sentinel values inserted so far with the same timestamp as '_lastPoppedKey'.
    std::size_t _sentinelCount = 0;

    Timestamp _lastPushedTimestamp;

    BSONObj _lastPoppedKey;
};

}  // namespace repl
}  // namespace mongo
