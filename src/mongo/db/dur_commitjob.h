/* @file dur_commitjob.h used by dur.cpp */

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

#include "mongo/db/cmdline.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/dur.h"
#include "mongo/db/durop.h"
#include "mongo/db/taskqueue.h"
#include "mongo/util/alignedbuilder.h"
#include "mongo/util/concurrency/synchronization.h"
#include "mongo/util/mongoutils/hash.h"

namespace mongo {
    namespace dur {

        void assertLockedForCommitting();

        /** Declaration of an intent to write to a region of a memory mapped view
         *  We store the end rather than the start pointer to make operator< faster
         *    since that is heavily used in set lookup.
         */
        struct WriteIntent { /* copyable */
            WriteIntent() : p(0) { }
            WriteIntent(void *a, unsigned b) : p((char*)a+b), len(b) { }
            void* start() const                            { return (char*)p - len; }
            void* end() const                              { return p; }
            unsigned length() const                        { return len; }
            bool operator < (const WriteIntent& rhs) const { return end() < rhs.end(); }
            bool overlaps(const WriteIntent& rhs) const    { return (start() <= rhs.end() && end() >= rhs.start()); }
            bool contains(const WriteIntent& rhs) const    { return (start() <= rhs.start() && end() >= rhs.end()); }
            // merge into me:
            void absorb(const WriteIntent& other);
            friend ostream& operator << (ostream& out, const WriteIntent& wi) {
                return (out << "p: " << wi.p << " end: " << wi.end() << " len: " << wi.len);
            }
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
        class IntentsAndDurOps : boost::noncopyable {
        public:
            vector<WriteIntent> _intents;
            Already<127> _alreadyNoted;
            vector< shared_ptr<DurOp> > _durOps; // all the ops other than basic writes

            /** reset the IntentsAndDurOps structure (empties all the above) */
            void clear();

            void insertWriteIntent(void* p, int len) {
                _intents.push_back(WriteIntent(p,len));
                wassert( _intents.size() < 2000000 );
            }
            #if defined(DEBUG_WRITE_INTENT)
            map<void*,int> _debug;
            #endif
        };

        /** so we don't have to lock the groupCommitMutex too often */
        class ThreadLocalIntents {
            enum { N = 21 };
            std::vector<dur::WriteIntent> intents;
            bool condense();
        public:
            ThreadLocalIntents() : intents(N) { intents.clear(); }
            ~ThreadLocalIntents();
            void _unspool();
            void unspool();
            void push(const WriteIntent& i);
            int n_informational() const { return intents.size(); }
            static AtomicUInt nSpooled;
        };

        /** A commit job object for a group commit.  Currently there is one instance of this object.

            concurrency: assumption is caller is appropriately locking.
                         for example note() invocations are from the write lock.
                         other uses are in a read lock from a single thread (durThread)
        */
        class CommitJob : boost::noncopyable {
            void _committingReset();
            ~CommitJob(){ verify(!"shouldn't destroy CommitJob!"); }

            /** record/note an intent to write */
            void note(void* p, int len);
            // only called by : 
            friend class ThreadLocalIntents;

        public:
            SimpleMutex groupCommitMutex;
            CommitJob();

            /** note an operation other than a "basic write". threadsafe (locks in the impl) */
            void noteOp(shared_ptr<DurOp> p);

            vector< shared_ptr<DurOp> >& ops() { 
                dassert( Lock::isLocked() );          // a rather weak check, we require more than that
                groupCommitMutex.dassertLocked(); // this is what really makes the below safe
                return _intentsAndDurOps._durOps;                
            }

            /** this method is safe to call outside of locks. when haswritten is false we don't do any group commit and avoid even
                trying to acquire a lock, which might be helpful at times.
            */
            bool hasWritten() const { return _hasWritten; }

        public:
            /** these called by the groupCommit code as it goes along */
            void commitingBegin();
            /** the commit code calls this when data reaches the journal (on disk) */
            void committingNotifyCommitted() { 
                groupCommitMutex.dassertLocked();
                _notify.notifyAll(_commitNumber); 
            }
            /** we use the commitjob object over and over, calling reset() rather than reconstructing */
            void committingReset() {
                groupCommitMutex.dassertLocked();
                _committingReset();
            }

        public:
            /** we check how much written and if it is getting to be a lot, we commit sooner. */
            size_t bytes() const { return _bytes; }

            /** used in prepbasicwrites. sorted so that overlapping and duplicate items 
             * can be merged.  we sort here so the caller receives something they must 
             * keep const from their pov. */
            const vector<WriteIntent>& getIntentsSorted() {
                groupCommitMutex.dassertLocked();
                sort(_intentsAndDurOps._intents.begin(), _intentsAndDurOps._intents.end());
                return _intentsAndDurOps._intents;
            }

            bool _hasWritten;

        private:
            NotifyAll::When _commitNumber;
            IntentsAndDurOps _intentsAndDurOps;
            size_t _bytes;
        public:
            NotifyAll _notify;                  // for getlasterror fsync:true acknowledgements
            unsigned _nSinceCommitIfNeededCall; // for asserts and debugging
        };

        extern CommitJob& commitJob;

#if defined(DEBUG_WRITE_INTENT)
        void assertAlreadyDeclared(void *, int len);
#else
        inline void assertAlreadyDeclared(void *, int len) { }
#endif

    }
}
