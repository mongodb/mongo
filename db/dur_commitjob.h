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
#include "durop.h"

namespace mongo { 
    namespace dur {

        /* declaration of an intent to write to a region of a memory mapped view */
        struct WriteIntent /* copyable */ { 
            WriteIntent() : w_ptr(0), p(0) { }
            WriteIntent(void *a, unsigned b) : w_ptr(0), p(a), len(b) { }
            void *w_ptr;  // p is mapped from private to equivalent location in the writable mmap
            void *p;      // intent to write at p
            unsigned len; // up to this len
        };

        /* try to remember things we have already marked for journalling.  false negatives are ok if infrequent - 
           we will just log them twice.
           */
        template<int Prime>
        class Already {
            enum { N = Prime }; // this should be small the idea is that it fits in the cpu cache easily
            WriteIntent nodes[N];
        public:
            Already() { clear(); }
            void clear() { memset(this, 0, sizeof(*this)); }

            /* see if we have Already recorded/indicated our write intent for this region of memory.
               @return true if already indicated.
            */
            bool checkAndSet(const WriteIntent& w) {
                unsigned x = mongoutils::hashPointer(w.p);
                WriteIntent& nd = nodes[x % N];
                if( nd.p != w.p || nd.len < w.len ) {
                    nd = w;
                    return false;
                }
                return true;
            }
        };

        /* our record of pending/uncommitted write intents */
        class Writes : boost::noncopyable {
        public:
            Already<127> _alreadyNoted;
            vector<WriteIntent> _writes;
            vector< shared_ptr<DurOp> > _ops; // all the ops other than basic writes

            void clear() { 
                _alreadyNoted.clear();
                _writes.clear();
                _ops.clear();
            }
        };

        /** concurrency: assumption is caller is appropriately locking.
                         for example note() invocations are from the write lock.
                         other uses are in a read lock from a single thread (durThread)
        */
        class CommitJob : boost::noncopyable { 
            bool _hasWritten;
            Writes _wi;
        public:
            AlignedBuilder _ab; // for direct i/o writes to journal

            CommitJob() : _hasWritten(false), _ab(4 * 1024 * 1024) { }

            /** record/note an intent to write */
            void note(WriteIntent& w) {
                // from the point of view of the dur module, it would be fine (i think) to only 
                // be read locked here.  but must be at least read locked to avoid race with 
                // remapprivateview
                DEV dbMutex.assertWriteLocked();
                if( !_wi._alreadyNoted.checkAndSet(w) ) {
                    if( !_hasWritten ) {
                        assert( !dbMutex._remapPrivateViewRequested );
                        _hasWritten = true;
                    }
                    // remember intent. we will journal it in a bit
                    _wi._writes.push_back(w);
                    wassert( _wi._writes.size() <  2000000 );
                    assert(  _wi._writes.size() < 20000000 );
                }
            }

            /** note an operation other than a "basic write" */
            void noteOp(shared_ptr<DurOp> p) {
                DEV dbMutex.assertWriteLocked();
                if( !_hasWritten ) {
                    assert( !dbMutex._remapPrivateViewRequested );
                    _hasWritten = true;
                }
                _wi._ops.push_back(p);
            }

            vector<WriteIntent>& writes() { return _wi._writes; }
            vector< shared_ptr<DurOp> >& ops() { return _wi._ops; }

            /** this method is safe to call outside of locks. when haswritten is false we don't do any group commit and avoid even 
                trying to acquire a lock, which might be helpful at times. 
            */
            bool hasWritten() const { return _hasWritten; }

            /** we use the commitjob object over and over, calling reset() rather than reconstructing */
            void reset() { 
                _hasWritten = false;
                _wi.clear();
                _ab.reset();
            }
        };

    }
}
