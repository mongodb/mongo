/* @file dur_commitjob.h used by dur.cpp
*/

/**
*    Copyright (C) 2009 10gen Inc.
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
*/

#pragma once

#include "../util/alignedbuilder.h"
#include "../util/mongoutils/hash.h"
#include "../util/concurrency/synchronization.h"
#include "cmdline.h"
#include "durop.h"
#include "dur.h"
#include "taskqueue.h"

//#define DEBUG_WRITE_INTENT 1

namespace mongo {
    namespace dur {

        /** declaration of an intent to write to a region of a memory mapped view
         *
         * We store the end rather than the start pointer to make operator< faster
         * since that is heavily used in set lookup.
         */
        struct WriteIntent { /* copyable */
            WriteIntent() : /*w_ptr(0), */ p(0) { }
            WriteIntent(void *a, unsigned b) : /*w_ptr(0), */ p((char*)a+b), len(b) { }

            void* start() const { return (char*)p - len; }
            void* end() const { return p; }
            unsigned length() const { return len; }

            bool operator < (const WriteIntent& rhs) const { return end() < rhs.end(); }

            // can they be merged?
            bool overlaps(const WriteIntent& rhs) const {
                return (start() <= rhs.end() && end() >= rhs.start());
            }

            // is merging necessary?
            bool contains(const WriteIntent& rhs) const {
                return (start() <= rhs.start() && end() >= rhs.end());
            }

            // merge into me
            void absorb(const WriteIntent& other);

            friend ostream& operator << (ostream& out, const WriteIntent& wi) {
                return (out << "p: " << wi.p << " end: " << wi.end() << " len: " << wi.len);
            }

            //mutable void *w_ptr;  // writable mapping of p.
            // mutable because set::iterator is const but this isn't used in op<
#if defined(_EXPERIMENTAL)
            mutable unsigned ofsInJournalBuffer;
#endif
        private:
            void *p;      // intent to write up to p
            unsigned len; // up to this len
        };

        /** try to remember things we have already marked for journaling.  false negatives are ok if infrequent -
            we will just log them twice.
        */
        template<int Prime>
        class Already : boost::noncopyable {
        public:
            Already() { clear(); }
            void clear() { memset(this, 0, sizeof(*this)); }

            /* see if we have Already recorded/indicated our write intent for this region of memory.
               automatically upgrades the length if the length was shorter previously.
               @return true if already indicated.
            */
            bool checkAndSet(void* p, int len) {
                unsigned x = mongoutils::hashPointer(p);
                pair<void*, int>& nd = nodes[x % N];
                if( nd.first == p ) {
                    if( nd.second < len ) {
                        nd.second = len;
                        return false; // haven't indicated this len yet
                    }
                    return true; // already indicated
                }
                nd.first = p;
                nd.second = len;
                return false; // a new set
            }

        private:
            enum { N = Prime }; // this should be small the idea is that it fits in the cpu cache easily
            pair<void*,int> nodes[N];
        };

        /** our record of pending/uncommitted write intents */
        class Writes : boost::noncopyable {
            struct D {
                void *p;
                unsigned len;
                static void go(const D& d);
            };
        public:
            TaskQueue<D> _deferred;
            Already<127> _alreadyNoted;
            set<WriteIntent> _writes;
            vector< shared_ptr<DurOp> > _ops; // all the ops other than basic writes
            bool _drained; // _deferred is drained?  for asserting/testing

            /** reset the Writes structure (empties all the above) */
            void clear();

            /** merges into set (ie non-deferred version) */
            void _insertWriteIntent(void* p, int len);

            void insertWriteIntent(void* p, int len) {
#if defined(DEBUG_WRITE_INTENT)
                if( _debug[p] < len )
                    _debug[p] = len;
#endif
                D d;
                d.p = p;
                d.len = len;
                _deferred.defer(d);
            }

#ifdef _DEBUG
            WriteIntent _last;
#endif
#if defined(DEBUG_WRITE_INTENT)
            map<void*,int> _debug;
#endif
        };

#if defined(DEBUG_WRITE_INTENT)
        void assertAlreadyDeclared(void *, int len);
#else
        inline void assertAlreadyDeclared(void *, int len) { }
#endif

        /** A commit job object for a group commit.  Currently there is one instance of this object.

            concurrency: assumption is caller is appropriately locking.
                         for example note() invocations are from the write lock.
                         other uses are in a read lock from a single thread (durThread)
        */
        class CommitJob : boost::noncopyable {
        public:
            AlignedBuilder _ab; // for direct i/o writes to journal

            CommitJob();

            ~CommitJob(){ assert(!"shouldn't destroy CommitJob!"); }

            /** record/note an intent to write */
            void note(void* p, int len);

            /** note an operation other than a "basic write" */
            void noteOp(shared_ptr<DurOp> p);

            set<WriteIntent>& writes() {
                if( !_wi._drained ) {
                    // generally, you don't want to use the set until it is prepared (after deferred ops are applied)
                    // thus this assert here.
                    assert(false);
                }
                return _wi._writes;
            }

            vector< shared_ptr<DurOp> >& ops() { return _wi._ops; }

            /** this method is safe to call outside of locks. when haswritten is false we don't do any group commit and avoid even
                trying to acquire a lock, which might be helpful at times.
            */
            bool hasWritten() const { return _hasWritten; }

            /** we use the commitjob object over and over, calling reset() rather than reconstructing */
            void reset();

            void beginCommit();

            /** the commit code calls this when data reaches the journal (on disk) */
            void notifyCommitted() { _notify.notifyAll(_commitNumber); }

            /** we check how much written and if it is getting to be a lot, we commit sooner. */
            size_t bytes() const { return _bytes; }

#if defined(_DEBUG)
            const WriteIntent& lastWrite() const { return _wi._last; }
#endif

            Writes& wi() { return _wi; }
        private:
            NotifyAll::When _commitNumber;
            bool _hasWritten;
            Writes _wi; // todo: fix name
            size_t _bytes;
        public:
            NotifyAll _notify; // for getlasterror fsync:true acknowledgements
            unsigned _nSinceCommitIfNeededCall;
        };

        extern CommitJob& commitJob;

    }
}
