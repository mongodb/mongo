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

namespace mongo {

    namespace dur {

        BOOST_STATIC_ASSERT( UncommittedBytesLimit > BSONObjMaxInternalSize * 3 );
        BOOST_STATIC_ASSERT( sizeof(void*)==4 || UncommittedBytesLimit > BSONObjMaxInternalSize * 6 );

        void Writes::D::go(const Writes::D& d) {
            commitJob.wi()._insertWriteIntent(d.p, d.len);
        }

        void WriteIntent::absorb(const WriteIntent& other) {
            dassert(overlaps(other));

            void* newStart = min(start(), other.start());
            p = max(p, other.p);
            len = (char*)p - (char*)newStart;

            dassert(contains(other));
        }

        void Writes::clear() {
            d.dbMutex.assertAtLeastReadLocked();

            _alreadyNoted.clear();
            _writes.clear();
            _ops.clear();
            _drained = false;
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

        void Writes::_insertWriteIntent(void* p, int len) {
            WriteIntent wi(p, len);

            if (_writes.empty()) {
                _writes.insert(wi);
                return;
            }

            typedef set<WriteIntent>::const_iterator iterator; // shorter

            iterator closest = _writes.lower_bound(wi);
            // closest.end() >= wi.end()

            if ((closest != _writes.end() && closest->overlaps(wi)) || // high end
                    (closest != _writes.begin() && (--closest)->overlaps(wi))) { // low end
                if (closest->contains(wi))
                    return; // nothing to do

                // find overlapping range and merge into wi
                iterator   end(closest);
                iterator begin(closest);
                while (  end->overlaps(wi)) { wi.absorb(*end); ++end; if (end == _writes.end()) break; }  // look forwards
                while (begin->overlaps(wi)) { wi.absorb(*begin); if (begin == _writes.begin()) break; --begin; } // look backwards
                if (!begin->overlaps(wi)) ++begin; // make inclusive

                DEV { // ensure we're not deleting anything we shouldn't
                    for (iterator it(begin); it != end; ++it) {
                        assert(wi.contains(*it));
                    }
                }

                _writes.erase(begin, end);
                _writes.insert(wi);

                DEV { // ensure there are no overlaps
                    // this can be very slow - n^2 - so make it RARELY
                    RARELY {
                        for (iterator it(_writes.begin()), end(boost::prior(_writes.end())); it != end; ++it) {
                            assert(!it->overlaps(*boost::next(it)));
                        }
                    }
                }
            }
            else { // no entries overlapping wi
                _writes.insert(closest, wi);
            }
        }

        /** note an operation other than a "basic write" */
        void CommitJob::noteOp(shared_ptr<DurOp> p) {
            d.dbMutex.assertWriteLocked();
            dassert( cmdLine.dur );
            cc().writeHappened();
            if( !_hasWritten ) {
                assert( !d.dbMutex._remapPrivateViewRequested );
                _hasWritten = true;
            }
            _wi._ops.push_back(p);
        }

        size_t privateMapBytes = 0; // used by _REMAPPRIVATEVIEW to track how much / how fast to remap

        void CommitJob::beginCommit() { 
            DEV d.dbMutex.assertAtLeastReadLocked();
            _commitNumber = _notify.now();
            stats.curr->_commits++;
        }

        void CommitJob::reset() {
            _hasWritten = false;
            _wi.clear();
            privateMapBytes += _bytes;
            _bytes = 0;
            _nSinceCommitIfNeededCall = 0;
        }

        CommitJob::CommitJob() : _ab(4 * 1024 * 1024) , _hasWritten(false), 
            _bytes(0), _nSinceCommitIfNeededCall(0) { 
            _commitNumber = 0;
        }

        extern unsigned notesThisLock;

        void CommitJob::note(void* p, int len) {
            // from the point of view of the dur module, it would be fine (i think) to only
            // be read locked here.  but must be at least read locked to avoid race with
            // remapprivateview
            DEV notesThisLock++;
            DEV d.dbMutex.assertWriteLocked();
            dassert( cmdLine.dur );
            cc().writeHappened();
            if( !_wi._alreadyNoted.checkAndSet(p, len) ) {
                MemoryMappedFile::makeWritable(p, len);

                if( !_hasWritten ) {
                    // you can't be writing if one of these is pending, so this is a verification.
                    assert( !d.dbMutex._remapPrivateViewRequested ); // safe to assert here since it must be the first write in a write lock

                    // we don't bother doing a group commit when nothing is written, so we have a var to track that
                    _hasWritten = true;
                }

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
                _wi.insertWriteIntent(p, len);
                wassert( _wi._writes.size() <  2000000 );
                //assert(  _wi._writes.size() < 20000000 );

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
                                if( _nSinceCommitIfNeededCall == 120 || _nSinceCommitIfNeededCall == 1200 ) {
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
