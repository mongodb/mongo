/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_writer_batcher.h"
#include "mongo/executor/task_executor.h"

namespace mongo {
namespace repl {

/**
 * Writes oplog entries to the oplog and/or the change collection.
 */
class OplogWriter {
    OplogWriter(const OplogWriter&) = delete;
    OplogWriter& operator=(const OplogWriter&) = delete;

public:
    /**
     * Used to configure the behavior of this OplogWriter.
     */
    class Options {
    public:
        Options() : skipWritesToOplogColl(false) {}
        explicit Options(bool skipWritesToOplogColl)
            : skipWritesToOplogColl(skipWritesToOplogColl) {}

        const bool skipWritesToOplogColl;
    };

    // Used to report oplog write progress.
    class Observer;

    /**
     * Constructs this OplogWriter with specific options.
     */
    OplogWriter(executor::TaskExecutor* executor, OplogBuffer* writeBuffer, const Options& options);

    virtual ~OplogWriter() = default;

    /**
     * Returns this writer's input buffer.
     */
    OplogBuffer* getBuffer() const;

    /**
     * Starts this OplogWriter.
     * Use the Future object to be notified when this OplogWriter has finished shutting down.
     */
    Future<void> startup();

    /**
     * Starts the shutdown process for this OplogWriter.
     * It is safe to call shutdown() multiple times.
     */
    void shutdown();

    /**
     * Returns true if this OplogWriter is shutting down.
     */
    bool inShutdown() const;

    /**
     * Pushes operations read into oplog buffer.
     */
    void enqueue(OperationContext* opCtx,
                 OplogBuffer::Batch::const_iterator begin,
                 OplogBuffer::Batch::const_iterator end,
                 boost::optional<std::size_t> bytes = boost::none);

    /**
     * Writes a batch of oplog entries to the oplog and/or the change collection.
     *
     * If the batch write is successful, returns the optime of the last op written,
     * which should be the last op in the batch.
     *
     * Oplog visibility and updates to replication coordinator timestamps should be
     * handled by caller.
     */
    virtual StatusWith<OpTime> writeOplogBatch(OperationContext* opCtx,
                                               const std::vector<BSONObj>& ops) = 0;

    const Options& getOptions() const;

private:
    /**
     * Called from startup() to run oplog write loop.
     * Currently applicable to steady state replication only.
     * Implemented in subclasses but not visible otherwise.
     */
    virtual void _run() = 0;

protected:
    OplogWriterBatcher _batcher;

private:
    // Protects member data of this OplogWriter.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("OplogWriter::_mutex");

    // Used to schedule task for oplog write loop.
    // Not owned by us.
    executor::TaskExecutor* const _executor;

    // Not owned by us.
    OplogBuffer* const _writeBuffer;

    // Set to true if shutdown() has been called.
    bool _inShutdown = false;

    // Configures this OplogWriter.
    const Options _options;
};

/**
 * The OplogWriter reports its progress using the Observer interface.
 */
class OplogWriter::Observer {
public:
    virtual ~Observer() = default;

    virtual void onWriteOplogCollection(const std::vector<InsertStatement>& docs) = 0;
    virtual void onWriteChangeCollection(const std::vector<InsertStatement>& docs) = 0;
};

/**
 * An Observer implementation that does nothing.
 */
class NoopOplogWriterObserver : public OplogWriter::Observer {
public:
    void onWriteOplogCollection(const std::vector<InsertStatement>& docs) final {}
    void onWriteChangeCollection(const std::vector<InsertStatement>& docs) final {}
};

extern NoopOplogWriterObserver noopOplogWriterObserver;

}  // namespace repl
}  // namespace mongo
