/* @file dur_commitjob.cpp */

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

#include "pch.h"
#include "dur_commitjob.h"
#include "dur_stats.h"
#include "taskqueue.h"
#include "client.h"
#include "../util/concurrency/threadlocal.h"

namespace mongo {

#if defined(_DEBUG) && (defined(_WIN64) || !defined(_WIN32))
#define CHECK_SPOOLING 1
#endif

    namespace dur {
       void ThreadLocalIntents::push(const WriteIntent& x) { 
            if( !commitJob._hasWritten )
                commitJob._hasWritten = true;
            if( n == 21 )
                unspool();
            i[n++] = x;
#if( CHECK_SPOOLING )
            nSpooled++;
#endif
        }
        void ThreadLocalIntents::_unspool() {
            if( n ) { 
                for( int j = 0; j < n; j++ )
                    commitJob.note(i[j].start(), i[j].length());
#if( CHECK_SPOOLING )
                nSpooled.signedAdd(-n);
#endif
                n = 0;
                dassert( cmdLine.dur );
            }
        }
        void ThreadLocalIntents::unspool() {
            if( n ) { 
                SimpleMutex::scoped_lock lk(commitJob.groupCommitMutex);
                _unspool();
            }
        }
        AtomicUInt ThreadLocalIntents::nSpooled;
    }

    TSP_DECLARE(dur::ThreadLocalIntents,tlIntents)
    TSP_DEFINE(dur::ThreadLocalIntents,tlIntents)

    namespace dur {

        void assertNothingSpooled() { 
#if( CHECK_SPOOLING )
            if( ThreadLocalIntents::nSpooled != 0 ) {
                log() << ThreadLocalIntents::nSpooled.get() << endl;
                if( tlIntents.get() )
                    log() << "me:" << tlIntents.get()->n_informational() << endl;
                else 
                    log() << "no tlIntent for my thread" << endl;
                verify(false);
            }
#endif
        }
        // when we release our w or W lock this is invoked
        void unspoolWriteIntents() { 
            ThreadLocalIntents *t = tlIntents.get();
            if( t ) 
                t->unspool();
        }

        /** base declare write intent function that all the helpers call. */
        /** we batch up our write intents so that we do not have to synchronize too often */
        void DurableImpl::declareWriteIntent(void *p, unsigned len) {
            cc().writeHappened();
            MemoryMappedFile::makeWritable(p, len);
            ThreadLocalIntents *t = tlIntents.getMake();
            t->push(WriteIntent(p,len));
        }

        BOOST_STATIC_ASSERT( UncommittedBytesLimit > BSONObjMaxInternalSize * 3 );
        BOOST_STATIC_ASSERT( sizeof(void*)==4 || UncommittedBytesLimit > BSONObjMaxInternalSize * 6 );

        void WriteIntent::absorb(const WriteIntent& other) {
            dassert(overlaps(other));

            void* newStart = min(start(), other.start());
            p = max(p, other.p);
            len = (char*)p - (char*)newStart;

            dassert(contains(other));
        }

        void IntentsAndDurOps::clear() {
            assertLockedForCommitting();
            commitJob.groupCommitMutex.dassertLocked();
            _alreadyNoted.clear();
            _intents.clear();
            _durOps.clear();
#if defined(DEBUG_WRITE_INTENT)
            cout << "_debug clear\n";
            _debug.clear();
#endif
        }

#if defined(DEBUG_WRITE_INTENT)
        void assertAlreadyDeclared(void *p, int len) {
            if( commitJob.wi()._debug[p] >= len )
                return;
            log() << "assertAlreadyDeclared fails " << (void*)p << " len:" << len << ' ' << commitJob.wi()._debug[p] << endl;
            printStackTrace();
            abort();
        }
#endif

        /** note an operation other than a "basic write" */
        void CommitJob::noteOp(shared_ptr<DurOp> p) {
            dassert( cmdLine.dur );
            // DurOp's are rare so it is ok to have the lock cost here
            SimpleMutex::scoped_lock lk(groupCommitMutex);
            cc().writeHappened();
            _hasWritten = true;
            _intentsAndDurOps._durOps.push_back(p);
        }

        size_t privateMapBytes = 0; // used by _REMAPPRIVATEVIEW to track how much / how fast to remap

        void CommitJob::commitingBegin() { 
            assertLockedForCommitting();
            _commitNumber = _notify.now();
            stats.curr->_commits++;
        }

        void CommitJob::_committingReset() {
            _hasWritten = false;
            _intentsAndDurOps.clear();
            privateMapBytes += _bytes;
            _bytes = 0;
            _nSinceCommitIfNeededCall = 0;
        }

        CommitJob::CommitJob() : 
            groupCommitMutex("groupCommit"),
            _hasWritten(false)
        { 
            _commitNumber = 0;
            _bytes = 0;
            _nSinceCommitIfNeededCall = 0;
        }

        void CommitJob::note(void* p, int len) {
            groupCommitMutex.dassertLocked();

            dassert( _hasWritten );

            // from the point of view of the dur module, it would be fine (i think) to only
            // be read locked here.  but must be at least read locked to avoid race with
            // remapprivateview

            if( !_intentsAndDurOps._alreadyNoted.checkAndSet(p, len) ) {

                /** tips for debugging:
                        if you have an incorrect diff between data files in different folders
                        (see jstests/dur/quick.js for example),
                        turn this on and see what is logged.  if you have a copy of its output from before the
                        regression, a simple diff of these lines would tell you a lot likely.
                */
#if 0 && defined(_DEBUG)
                {
                    static int n;
                    if( ++n < 10000 ) {
                        size_t ofs;
                        MongoMMF *mmf = privateViews._find(w.p, ofs);
                        if( mmf ) {
                            log() << "DEBUG note write intent " << w.p << ' ' << mmf->filename() << " ofs:" << hex << ofs << " len:" << w.len << endl;
                        }
                        else {
                            log() << "DEBUG note write intent " << w.p << ' ' << w.len << " NOT FOUND IN privateViews" << endl;
                        }
                    }
                    else if( n == 10000 ) {
                        log() << "DEBUG stopping write intent logging, too much to log" << endl;
                    }
                }
#endif

                // remember intent. we will journal it in a bit
                _intentsAndDurOps.insertWriteIntent(p, len);

                {
                    // a bit over conservative in counting pagebytes used
                    static size_t lastPos; // note this doesn't reset with each commit, but that is ok we aren't being that precise
                    size_t x = ((size_t) p) & ~0xfff; // round off to page address (4KB)
                    if( x != lastPos ) { 
                        lastPos = x;
                        unsigned b = (len+4095) & ~0xfff;
                        _bytes += b;
#if defined(_DEBUG)
                        _nSinceCommitIfNeededCall++;
                        if( _nSinceCommitIfNeededCall >= 80 ) {
                            if( _nSinceCommitIfNeededCall % 40 == 0 ) {
                                log() << "debug nsincecommitifneeded:" << _nSinceCommitIfNeededCall << " bytes:" << _bytes << endl;
                                if( _nSinceCommitIfNeededCall == 240 || _nSinceCommitIfNeededCall == 1200 ) {
                                    log() << "_DEBUG printing stack given high nsinccommitifneeded number" << endl;
                                    printStackTrace();
                                }
                            }
                        }
#endif
                        if (_bytes > UncommittedBytesLimit * 3) {
                            static time_t lastComplain;
                            static unsigned nComplains;
                            // throttle logging
                            if( ++nComplains < 100 || time(0) - lastComplain >= 60 ) {
                                lastComplain = time(0);
                                warning() << "DR102 too much data written uncommitted " << _bytes/1000000.0 << "MB" << endl;
                                if( nComplains < 10 || nComplains % 10 == 0 ) {
                                    // wassert makes getLastError show an error, so we just print stack trace
                                    printStackTrace();
                                }
                            }
                        }
                    }
                }
            }
        }

    }
}
