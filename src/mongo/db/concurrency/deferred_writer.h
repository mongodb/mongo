/**
 *    Copyright (C) 2017 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/db/catalog/collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class AutoGetCollection;
class ThreadPool;

/**
 * Provides an interface for asynchronously adding to a collection.
 *
 * Allows writes to a collection in a context without appropriate locks by buffering them in memory
 * and asynchronously writing them to the backing collection.  Useful when an operation with e.g. a
 * global MODE_S lock needs to write, but doesn't care that the write shows up immediately.
 * Motivated by the local health log.  For obvious reasons, cannot provide strong durability
 * guarantees, and cannot report whether the insert succeeded--in other words, this class provides
 * eventual "best effort" inserts.
 *
 * Because this class is motivated by the health log and errors cannot be cleanly reported to the
 * caller, it cannot report most errors to the client; it instead periodically logs any errors to
 * the system log.
 *
 * Instances of this class are unconditionally thread-safe, and cannot cause deadlock barring
 * improper use of the ctor, `flush` and `shutdown` methods below.
 */
class DeferredWriter {
    MONGO_DISALLOW_COPYING(DeferredWriter);

public:
    /**
     * Create a new DeferredWriter for writing to a given collection.
     *
     * Will not begin writing to the backing collection until `startup` is called.
     *
     * @param opts The options to use when creating the backing collection if it doesn't exist.
     * @param maxSize the maximum number of bytes to store in the buffer.
     */
    DeferredWriter(NamespaceString nss, CollectionOptions opts, int64_t maxSize);

    /**
     * Start the background worker thread writing to the given collection.
     *
     * @param workerName The name of the client associated with the worker thread.
     */
    void startup(std::string workerName);

    /**
     * Flush the buffer and `join` the worker thread.
     *
     * IMPORTANT: Must be called before destruction if `startup` has been called.
     *
     * Blocks until buffered writes complete.  Must not be called repeatedly.
     */
    void shutdown(void);

    /**
     * Cleans up the writer.
     *
     * Does not clean up the worker thread; call `shutdown` for that.  Instead, if the worker thread
     * is still running calls std::terminate, which crashes the server.
     */
    ~DeferredWriter();

    /**
     * Deferred-insert the given object.
     *
     * Returns whether the object was successfully pushed onto the in-memory buffer (*not* whether
     * it was successfully added to the underlying collection).  Creates the backing collection if
     * it doesn't exist.
     */
    bool insertDocument(BSONObj obj);

    /**
     * Get the number of dropped writes due to a full buffer since the last log
     */
    int64_t getDroppedEntries();

private:
    /**
     * Log failure, but only if a certain interval has passed since the last log.
     */
    void _logFailure(const Status& status);

    /**
     * Log number of entries dropped because of a full buffer. Rate limited and
     * each successful log resets the counter.
     */
    void _logDroppedEntry();

    /**
     * Create the backing collection if it doesn't exist.
     *
     * Return whether creation succeeded.
     */
    Status _makeCollection(OperationContext* opCtx);

    /**
     * Ensure that the backing collection exists, and pass back a lock and handle to it.
     */
    StatusWith<std::unique_ptr<AutoGetCollection>> _getCollection(OperationContext* opCtx);

    /**
     * The method that the worker thread will run.
     */
    void _worker(InsertStatement stmt);

    /**
     * The options for the collection, in case we need to create it.
     */
    const CollectionOptions _collectionOptions;

    /**
     * The size limit of the in-memory buffer.
     */
    const int64_t _maxNumBytes;

    /**
     * The name of the backing collection.
     */
    const NamespaceString _nss;

    std::unique_ptr<ThreadPool> _pool;

    /**
     * Guards all non-const, non-thread-safe members.
     */
    stdx::mutex _mutex;

    /**
     * The number of bytes currently in the in-memory buffer.
     */
    int64_t _numBytes;

    /**
     * The number of deffered entries that have been dropped. Resets when the
     * rate-limited system log is written out.
     */
    int64_t _droppedEntries;

    /**
     * Time we last logged that we can't write to the underlying collection.
     *
     * Ensures we don't flood the log with such entries.
     */
    using TimePoint = stdx::chrono::time_point<stdx::chrono::system_clock>;
    TimePoint _lastLogged;
    TimePoint _lastLoggedDrop;
};

}  // namespace mongo
