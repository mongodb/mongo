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


#include "mongo/db/storage/mmap_v1/durop.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {
namespace dur {

typedef std::vector<std::shared_ptr<DurOp>> DurOpsVector;

/**
 * Declaration of an intent to write to a region of a memory mapped view. We store the end
 * rather than the start pointer to make operator < faster since that is heavily used in
 * set lookup.
 */
struct WriteIntent {
    WriteIntent() : p(0) {}
    WriteIntent(void* a, unsigned b) : p((char*)a + b), len(b) {}

    void* start() const {
        return (char*)p - len;
    }
    void* end() const {
        return p;
    }
    unsigned length() const {
        return len;
    }
    bool operator<(const WriteIntent& rhs) const {
        return end() < rhs.end();
    }

    bool overlaps(const WriteIntent& rhs) const {
        return (start() <= rhs.end() && end() >= rhs.start());
    }

    bool contains(const WriteIntent& rhs) const {
        return (start() <= rhs.start() && end() >= rhs.end());
    }

    // merge into me:
    void absorb(const WriteIntent& other);

    friend std::ostream& operator<<(std::ostream& out, const WriteIntent& wi) {
        return (out << "p: " << wi.p << " end: " << wi.end() << " len: " << wi.len);
    }

private:
    void* p;       // intent to write up to p
    unsigned len;  // up to this len
};

typedef std::vector<WriteIntent> WriteIntentsVector;


/**
    * Bitmap to remember things we have already marked for journaling. False negatives are ok
    * if infrequent, since they impact performance.
    */
template <int Prime>
class Already {
    MONGO_DISALLOW_COPYING(Already);

public:
    Already() {
        clear();
    }

    void clear() {
        memset(this, 0, sizeof(*this));
    }

    /**
        * Checks if we have Already recorded/indicated our write intent for this region of
        * memory and automatically upgrades the length if the length was shorter previously.
        *
        *  @return true if already indicated.
        */
    bool checkAndSet(void* p, int len) {
        const unsigned x = hashPointer(p);
        std::pair<void*, int>& nd = nodes[x % Prime];

        if (nd.first == p) {
            if (nd.second < len) {
                nd.second = len;
                return false;  // haven't indicated this len yet
            }
            return true;  // already indicated
        }

        nd.first = p;
        nd.second = len;
        return false;  // a new set
    }

private:
    static unsigned hashPointer(void* v) {
        unsigned x = 0;
        unsigned char* p = (unsigned char*)&v;
        for (unsigned i = 0; i < sizeof(void*); i++) {
            x = x * 131 + p[i];
        }
        return x;
    }

    std::pair<void*, int> nodes[Prime];
};


/**
 * Tracks all write operations on the private view so they can be journaled.
 */
class CommitJob {
    MONGO_DISALLOW_COPYING(CommitJob);

public:
    CommitJob();
    ~CommitJob();

    /**
        * Note an operation other than a "basic write".
        */
    void noteOp(std::shared_ptr<DurOp> p);

    /**
        * Record/note an intent to write.
        *
        * NOTE: Not thread safe. Requires the mutex to be locked.
        */
    void note(void* p, int len);

    /**
     * When this value is false we don't have to do any group commit.
     */
    bool hasWritten() const {
        return _hasWritten;
    }

    /**
     * We use the commitjob object over and over, calling committingReset() rather than
     * reconstructing.
     */
    void committingReset();

    /**
     * We check how much written and if it is getting to be a lot, we commit sooner.
     */
    size_t bytes() const {
        return _bytes;
    }

    /**
     * Sorts the internal list of write intents so that overlapping and duplicate items can be
     * merged. We do the sort here so the caller receives something they must keep const from
     * their POV.
     */
    const WriteIntentsVector& getIntentsSorted() {
        sort(_intents.begin(), _intents.end());
        return _intents;
    }

    const DurOpsVector& ops() const {
        return _durOps;
    }

    SimpleMutex groupCommitMutex;

private:
    void _insertWriteIntent(void* p, int len) {
        _intents.push_back(WriteIntent(p, len));
        wassert(_intents.size() < 2000000);
    }


    // Whether we put write intents or durops
    bool _hasWritten;

    // Write intents along with a bitmask for whether we have already noted them
    Already<127> _alreadyNoted;
    WriteIntentsVector _intents;

    // All the ops other than basic writes
    DurOpsVector _durOps;

    // Used to count the private map used bytes. Note that _lastNotedPos doesn't reset with
    // each commit, but that is ok we aren't being that precise.
    size_t _lastNotedPos;
    size_t _bytes;

    // Warning logging for large commits
    uint64_t _lastComplainMs;
    unsigned _complains;
};

}  // namespace "dur"
}  // namespace "mongo"
