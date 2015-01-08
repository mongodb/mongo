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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/dur_commitjob.h"

#include <boost/shared_ptr.hpp>
#include <iostream>

#include "mongo/db/storage/mmap_v1/durable_mapped_file.h"
#include "mongo/db/storage/mmap_v1/dur_stats.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/log.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

    using boost::shared_ptr;

    namespace dur {

        /** base declare write intent function that all the helpers call. */
        /** we batch up our write intents so that we do not have to synchronize too often */
        void DurableImpl::declareWriteIntent(void *p, unsigned len) {
            privateViews.makeWritable(p, len);
            SimpleMutex::scoped_lock lk(commitJob.groupCommitMutex);
            commitJob.note(p, len);
        }

        void DurableImpl::declareWriteIntents(
                const std::vector<std::pair<void*, unsigned> >& intents) {
            typedef std::vector<std::pair<void*, unsigned> > Intents;
            SimpleMutex::scoped_lock lk(commitJob.groupCommitMutex);
            for (Intents::const_iterator it(intents.begin()), end(intents.end()); it != end; ++it) {
                commitJob.note(it->first, it->second);
            }
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
            dassert(storageGlobalParams.dur);
            // DurOp's are rare so it is ok to have the lock cost here
            SimpleMutex::scoped_lock lk(groupCommitMutex);
            _hasWritten = true;
            _intentsAndDurOps._durOps.push_back(p);
        }

        void CommitJob::committingReset() {
            _hasWritten = false;
            _intentsAndDurOps.clear();
            _bytes = 0;
        }

        CommitJob::CommitJob() : 
            groupCommitMutex("groupCommit"),
            _hasWritten(false),
            _lastNotedPos(0),
            _bytes(0) {

        }

        void CommitJob::note(void* p, int len) {
            _hasWritten = true;

            // from the point of view of the dur module, it would be fine (i think) to only
            // be read locked here.  but must be at least read locked to avoid race with
            // remapprivateview

            if (!_intentsAndDurOps._alreadyNoted.checkAndSet(p, len)) {

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
                        DurableMappedFile *mmf = privateViews._find(w.p, ofs);
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

                // Remember intent. We will journal it in a bit.
                _intentsAndDurOps.insertWriteIntent(p, len);

                // Round off to page address (4KB)
                const size_t x = ((size_t)p) & ~0xfff;

                if (x != _lastNotedPos) {
                    _lastNotedPos = x;
                    unsigned b = (len + 4095) & ~0xfff;
                    _bytes += b;

                    if (_bytes > UncommittedBytesLimit * 3) {
                        static time_t lastComplain;
                        static unsigned nComplains;
                        // throttle logging
                        if (++nComplains < 100 || time(0) - lastComplain >= 60) {
                            lastComplain = time(0);
                            warning() << "DR102 too much data written uncommitted " << _bytes / 1000000.0 << "MB" << endl;
                            if (nComplains < 10 || nComplains % 10 == 0) {
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
