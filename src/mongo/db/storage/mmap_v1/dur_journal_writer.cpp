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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kJournal

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/dur_journal_writer.h"

#include "mongo/db/client.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/dur_journal.h"
#include "mongo/db/storage/mmap_v1/dur_recover.h"
#include "mongo/db/storage/mmap_v1/dur_stats.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace dur {

namespace {

/**
 * Apply the writes back to the non-private MMF after they are for certain in the journal.
 *
 * (1) TODO we don't need to write back everything every group commit. We MUST write back that
 *  which is going to be a remapped on its private view - but that might not be all views.
 *
 * (2) TODO should we do this using N threads? Would be quite easy see Hackenberg paper table
 *  5 and 6. 2 threads might be a good balance.
 */
void WRITETODATAFILES(const JSectHeader& h, const AlignedBuilder& uncompressed) {
    Timer t;

    LOG(4) << "WRITETODATAFILES BEGIN";

    RecoveryJob::get().processSection(&h, uncompressed.buf(), uncompressed.len(), NULL);

    const long long m = t.micros();
    stats.curr()->_writeToDataFilesMicros += m;

    setLastSeqNumberWrittenToSharedView(h.seqNumber);

    LOG(4) << "journal WRITETODATAFILES " << m / 1000.0 << "ms";
}

}  // namespace


/**
 * Used inside the journal writer thread to ensure that used buffers are cleaned up properly.
 */
class BufferGuard {
    MONGO_DISALLOW_COPYING(BufferGuard);

public:
    BufferGuard(JournalWriter::Buffer* buffer, JournalWriter::BufferQueue* bufferQueue)
        : _buffer(buffer), _bufferQueue(bufferQueue) {}

    ~BufferGuard() {
        // This buffer is done. Reset and remove it from the journal queue and put it on
        // the ready queue.
        _buffer->_reset();

        // This should never block. Otherwise we will stall the journaling pipeline
        // permanently and cause deadlock.
        invariant(_bufferQueue->count() < _bufferQueue->maxSize());
        _bufferQueue->push(_buffer);
    }

private:
    // Buffer that this scoped object is managing. Owned until destruction time. Then, the
    // bufferQueue owns it.
    JournalWriter::Buffer* const _buffer;

    // Queue where the buffer should be returned to at destruction time. Not owned.
    JournalWriter::BufferQueue* const _bufferQueue;
};


//
// JournalWriter
//

JournalWriter::JournalWriter(CommitNotifier* commitNotify,
                             CommitNotifier* applyToDataFilesNotify,
                             size_t numBuffers)
    : _commitNotify(commitNotify),
      _applyToDataFilesNotify(applyToDataFilesNotify),
      _shutdownRequested(false),
      _journalQueue(numBuffers),
      _lastCommitNumber(0),
      _readyQueue(numBuffers) {
    invariant(_journalQueue.maxSize() == _readyQueue.maxSize());
}

JournalWriter::~JournalWriter() {
    // Never close the journal writer with outstanding or unaccounted writes
    invariant(_journalQueue.empty());
    invariant(_readyQueue.empty());
}

void JournalWriter::start() {
    // Do not allow reuse
    invariant(!_shutdownRequested);

    // Pre-allocate the journal buffers and push them on the ready queue
    for (size_t i = 0; i < _readyQueue.maxSize(); i++) {
        _readyQueue.push(new Buffer(InitialBufferSizeBytes));
    }

    // Start the thread
    stdx::thread t(stdx::bind(&JournalWriter::_journalWriterThread, this));
    _journalWriterThreadHandle.swap(t);
}

void JournalWriter::shutdown() {
    // There is no reason to call shutdown multiple times
    invariant(!_shutdownRequested);
    _shutdownRequested = true;

    // Never terminate the journal writer with outstanding or unaccounted writes
    assertIdle();

    Buffer* const shutdownBuffer = newBuffer();
    shutdownBuffer->_setShutdown();

    // This will terminate the journal thread. No need to specify commit number, since we are
    // shutting down and nothing will be notified anyways.
    writeBuffer(shutdownBuffer, 0);

    // Ensure the journal thread has stopped and everything accounted for.
    _journalWriterThreadHandle.join();
    assertIdle();

    // Delete the buffers (this deallocates the journal buffer memory)
    while (!_readyQueue.empty()) {
        Buffer* const buffer = _readyQueue.blockingPop();
        delete buffer;
    }
}

void JournalWriter::assertIdle() {
    // All buffers are in the ready queue means there is nothing pending.
    invariant(_journalQueue.empty());
    invariant(_readyQueue.count() == _readyQueue.maxSize());
}

JournalWriter::Buffer* JournalWriter::newBuffer() {
    Buffer* const buffer = _readyQueue.blockingPop();
    buffer->_assertEmpty();

    return buffer;
}

void JournalWriter::writeBuffer(Buffer* buffer, CommitNotifier::When commitNumber) {
    invariant(buffer->_commitNumber == 0);
    invariant((commitNumber > _lastCommitNumber) || (buffer->_isShutdown && (commitNumber == 0)));

    buffer->_commitNumber = commitNumber;

    _journalQueue.push(buffer);
}

void JournalWriter::flush() {
    std::vector<Buffer*> buffers;

    // Pop the expected number of buffers from the ready queue. This will block until all
    // in-progress buffers have completed.
    for (size_t i = 0; i < _readyQueue.maxSize(); i++) {
        buffers.push_back(_readyQueue.blockingPop());
    }

    // Put them back in to restore the original state.
    for (size_t i = 0; i < buffers.size(); i++) {
        _readyQueue.push(buffers[i]);
    }
}

void JournalWriter::_journalWriterThread() {
    Client::initThread("journal writer");

    log() << "Journal writer thread started";

    try {
        while (true) {
            Buffer* const buffer = _journalQueue.blockingPop();
            BufferGuard bufferGuard(buffer, &_readyQueue);

            if (buffer->_isShutdown) {
                invariant(buffer->_builder.len() == 0);

                // The journal writer thread is terminating. Nothing to notify or write.
                break;
            }

            if (buffer->_isNoop) {
                invariant(buffer->_builder.len() == 0);

                // There's nothing to be writen, but we still need to notify this commit number
                _commitNotify->notifyAll(buffer->_commitNumber);
                _applyToDataFilesNotify->notifyAll(buffer->_commitNumber);
                continue;
            }

            LOG(4) << "Journaling commit number " << buffer->_commitNumber << " (journal file "
                   << buffer->_header.fileId << ", sequence " << buffer->_header.seqNumber
                   << ", size " << buffer->_builder.len() << " bytes)";

            // This performs synchronous I/O to the journal file and will block.
            WRITETOJOURNAL(buffer->_header, buffer->_builder);

            // Data is now persisted in the journal, which is sufficient for acknowledging
            // durability.
            dur::getJournalListener()->onDurable(buffer->journalListenerToken);
            _commitNotify->notifyAll(buffer->_commitNumber);

            // Apply the journal entries on top of the shared view so that when flush is
            // requested it would write the latest.
            WRITETODATAFILES(buffer->_header, buffer->_builder);

            // Data is now persisted on the shared view, so notify any potential journal file
            // cleanup waiters.
            _applyToDataFilesNotify->notifyAll(buffer->_commitNumber);
        }
    } catch (const DBException& e) {
        severe() << "dbexception in journalWriterThread causing immediate shutdown: "
                 << e.toString();
        invariant(false);
    } catch (const std::ios_base::failure& e) {
        severe() << "ios_base exception in journalWriterThread causing immediate shutdown: "
                 << e.what();
        invariant(false);
    } catch (const std::bad_alloc& e) {
        severe() << "bad_alloc exception in journalWriterThread causing immediate shutdown: "
                 << e.what();
        invariant(false);
    } catch (const std::exception& e) {
        severe() << "exception in journalWriterThread causing immediate shutdown: " << e.what();
        invariant(false);
    } catch (...) {
        severe() << "unhandled exception in journalWriterThread causing immediate shutdown";
        invariant(false);
    }

    log() << "Journal writer thread stopped";
}


//
// Buffer
//

JournalWriter::Buffer::Buffer(size_t initialSize)
    : _commitNumber(0), _isNoop(false), _isShutdown(false), _header(), _builder(initialSize) {}

JournalWriter::Buffer::~Buffer() {
    _assertEmpty();
}

void JournalWriter::Buffer::_assertEmpty() {
    invariant(_commitNumber == 0);
    invariant(_builder.len() == 0);
}

void JournalWriter::Buffer::_reset() {
    _commitNumber = 0;
    _isNoop = false;
    _builder.reset();
}

}  // namespace dur
}  // namespace mongo
