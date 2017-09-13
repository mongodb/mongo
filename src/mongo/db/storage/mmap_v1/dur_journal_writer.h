/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/mmap_v1/aligned_builder.h"
#include "mongo/db/storage/mmap_v1/commit_notifier.h"
#include "mongo/db/storage/mmap_v1/dur_journalformat.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/queue.h"

namespace mongo {
namespace dur {

/**
 * Manages the thread and queues used for writing the journal to disk and notify parties with
 * are waiting on the write concern.
 *
 * NOTE: Not thread-safe and must not be used from more than one thread.
 */
class JournalWriter {
    MONGO_DISALLOW_COPYING(JournalWriter);

public:
    /**
     * Stores the memory and the header for a complete journal buffer which is pending to be
     * written by the journal writer thread.
     */
    class Buffer {
    public:
        Buffer(size_t initialSize);
        ~Buffer();

        JSectHeader& getHeader() {
            return _header;
        }
        AlignedBuilder& getBuilder() {
            return _builder;
        }

        void setNoop() {
            _isNoop = true;
        }

        JournalListener::Token journalListenerToken;

    private:
        friend class BufferGuard;
        friend class JournalWriter;


        void _assertEmpty();
        void _reset();
        void _setShutdown() {
            _isShutdown = true;
        }

        // Specifies the commit number which flushing this buffer would notify. This value is
        // zero, if there is no data to be flushed or if the buffer is noop/shutdown.
        CommitNotifier::When _commitNumber;

        // Special buffer that's posted when there is nothing to be written to the journal,
        // but we want to order a notification so it happens after all other writes have
        // completed.
        bool _isNoop;

        // Special buffer that's posted when the receiving thread must terminate. This should
        // be the last entry posted to the queue and the commit number should be zero.
        bool _isShutdown;

        JSectHeader _header;
        AlignedBuilder _builder;
    };


    /**
     * Initializes the journal writer.
     *
     * @param commitNotify Notification object to be called after journal entries have been
     *      written to disk. The caller retains ownership and the notify object must outlive
     *      the journal writer object.
     * @param applyToDataFilesNotify Notification object to be called after journal entries
     *      have been applied to the shared view. This means that if the shared view were to be
     *      flushed at this point, the journal files before this point are not necessary. The
     *      caller retains ownership and the notify object must outlive the journal writer
     *      object.
     * @param numBuffers How many buffers to create to hold outstanding writes. If there are
     *      more than this number of journal writes that have not completed, the write calls
     *      will block.
     */
    JournalWriter(CommitNotifier* commitNotify,
                  CommitNotifier* applyToDataFilesNotify,
                  size_t numBuffers);
    ~JournalWriter();

    /**
     * Allocates buffer memory and starts the journal writer thread.
     */
    void start();

    /**
     * Terminates the journal writer thread and frees memory for the buffers. Must not be
     * called if there are any pending journal writes.
     */
    void shutdown();

    /**
     * Asserts that there are no pending journal writes.
     */
    void assertIdle();

    /**
     * Obtains a new empty buffer into which a journal entry should be written.
     *
     * This method may block if there are no free buffers.
     *
     * The caller does not own the buffer and needs to "return" it to the writer by calling
     * writeBuffer. Buffers with data on them should never be discarded until they are written.
     */
    Buffer* newBuffer();

    /**
     * Requests that the specified buffer be written asynchronously.
     *
     * This method may block if there are too many outstanding unwritten buffers.
     *
     * @param buffer Buffer entry to be written. The buffer object must not be used anymore
     *      after it has been given to this function.
     * @param commitNumber What commit number to be notified once the buffer has been written
     *      to disk.
     */
    void writeBuffer(Buffer* buffer, CommitNotifier::When commitNumber);

    /**
     * Ensures that all previously submitted write requests complete. This call is blocking.
     */
    void flush();

private:
    friend class BufferGuard;

    typedef BlockingQueue<Buffer*> BufferQueue;

    // Start all buffers with 4MB of size
    enum { InitialBufferSizeBytes = 4 * 1024 * 1024 };


    void _journalWriterThread();


    // This gets notified as journal buffers are written. It is not owned and needs to outlive
    // the journal writer object.
    CommitNotifier* const _commitNotify;

    // This gets notified as journal buffers are done being applied to the shared view
    CommitNotifier* const _applyToDataFilesNotify;

    // Wraps and controls the journal writer thread
    stdx::thread _journalWriterThreadHandle;

    // Indicates that shutdown has been requested. Used for idempotency of the shutdown call.
    bool _shutdownRequested;

    // Queue of buffers, which need to be written by the journal writer thread
    BufferQueue _journalQueue;
    CommitNotifier::When _lastCommitNumber;

    // Queue of buffers, whose write has been completed by the journal writer thread.
    BufferQueue _readyQueue;
};

}  // namespace dur
}  // namespace mongo
