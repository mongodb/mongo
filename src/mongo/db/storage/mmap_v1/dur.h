/**
 *    Copyright (C) 2009-2014 MongoDB Inc.
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

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/storage/journal_listener.h"

namespace mongo {

class ClockSource;
class OperationContext;

namespace dur {

// a smaller limit is likely better on 32 bit
const unsigned UncommittedBytesLimit = (sizeof(void*) == 4) ? 50 * 1024 * 1024 : 512 * 1024 * 1024;

class DurableInterface {
    MONGO_DISALLOW_COPYING(DurableInterface);

public:
    virtual ~DurableInterface();

    /**
     * Declare that a file has been created. Normally writes are applied only after journaling
     * for safety. But here the file is created first, and the journal will just replay the
     * creation if the create didn't happen due to a crash.
     */
    virtual void createdFile(const std::string& filename, unsigned long long len) = 0;

    // Declare write intents. Use these methods to declare "i'm about to write to x and it
    // should be logged for redo."
    //
    // Failure to call declare write intents is checked in MONGO_CONFIG_DEBUG_BUILD mode by
    // using a read only mapped view (i.e., you'll segfault if the code is covered in that
    // situation).  The debug check doesn't verify that your length is correct though.
    virtual void declareWriteIntents(const std::vector<std::pair<void*, unsigned>>& intents) = 0;

    /** Wait for acknowledgement of the next group commit.
        @return true if --dur is on.  There will be delay.
        @return false if --dur is off.
        */
    virtual bool waitUntilDurable() = 0;

    /** Commit immediately.

        Generally, you do not want to do this often, as highly granular committing may affect
        performance.

        Does not return until the commit is complete.

        You must be at least read locked when you call this.  Ideally, you are not write locked
        and then read operations can occur concurrently.

        Do not use this. Use commitIfNeeded() instead.

        @return true if --dur is on.
        @return false if --dur is off. (in which case there is action)
        */
    virtual bool commitNow(OperationContext* txn) = 0;

    /** Commit if enough bytes have been modified. Current threshold is 50MB

        The idea is that long running write operations that don't yield
        (like creating an index or update with $atomic) can call this
        whenever the db is in a sane state and it will prevent commits
        from growing too large.
        @return true if commited
        */
    virtual bool commitIfNeeded() = 0;


    /**
     * Called when a DurableMappedFile is closing. Asserts that there are no unwritten changes,
     * because that would mean journal replay on recovery would try to write to non-existent
     * files and fail.
     */
    virtual void closingFileNotification() = 0;

    /**
        * Invoked at clean shutdown time. Performs one last commit/flush and terminates the
        * flush thread.
        *
        * Must be called under the global X lock.
        */
    virtual void commitAndStopDurThread() = 0;

    /**
     * Commits pending changes, flushes all changes to main data files, then removes the
     * journal.
     *
     * WARNING: Data *must* be in a crash-recoverable state when this is called and must
     *          not be inside of a write unit of work.
     *
     * This is useful as a "barrier" to ensure that writes before this call will never go
     * through recovery and be applied to files that have had changes made after this call
     * applied.
     */
    virtual void syncDataAndTruncateJournal(OperationContext* txn) = 0;

    virtual bool isDurable() const = 0;

    static DurableInterface& getDur() {
        return *_impl;
    }

protected:
    DurableInterface();

private:
    friend void startup(ClockSource* cs, int64_t serverStartMs);

    static DurableInterface* _impl;
};


/**
 * Called during startup to startup the durability module.
 * Does nothing if storageGlobalParams.dur is false
 */
void startup(ClockSource* cs, int64_t serverStartMs);

// Sets a new JournalListener, which is used to alert the rest of the system about
// journaled write progress.
void setJournalListener(JournalListener* jl);

// Expose the JournalListener, needed for the journal writer thread.
JournalListener* getJournalListener();

}  // namespace dur


/**
 * Provides a reference to the active durability interface.
 *
 * TODO: The only reason this is an inline function is that tests try to link it and fail if
 *       the MMAP V1 engine is not included.
 */
inline dur::DurableInterface& getDur() {
    return dur::DurableInterface::getDur();
}

}  // namespace mongo
