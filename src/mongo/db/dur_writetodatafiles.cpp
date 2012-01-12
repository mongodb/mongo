// @file dur_writetodatafiles.cpp apply the writes back to the non-private MMF after they are for certain in redo log

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
#include "dur_recover.h"
#include "../util/timer.h"

namespace mongo {
    namespace dur {

        void debugValidateAllMapsMatch();

        static void WRITETODATAFILES_Impl1(const JSectHeader& h, AlignedBuilder& uncompressed) {
            LockMongoFilesShared lk;
            LOG(3) << "journal WRITETODATAFILES 1" << endl;
            RecoveryJob::get().processSection(&h, uncompressed.buf(), uncompressed.len(), 0);
            LOG(3) << "journal WRITETODATAFILES 2" << endl;
        }

#if 0
        // the old implementation.  doesn't work with groupCommitWithLimitedLocks()
        void WRITETODATAFILES_Impl2() {
            /* we go backwards as what is at the end is most likely in the cpu cache.  it won't be much, but we'll take it. */
            for( set<WriteIntent>::const_iterator it(commitJob.writes().begin()), end(commitJob.writes().end()); it != end; ++it ) {
                const WriteIntent& intent = *it;
                stats.curr->_writeToDataFilesBytes += intent.length();
                dassert(intent.w_ptr);
                memcpy(intent.w_ptr, intent.start(), intent.length());
            }
        }
#endif

#if defined(_EXPERIMENTAL)
        // doesn't work with groupCommitWithLimitedLocks()
        void WRITETODATAFILES_Impl3() {
            /* we go backwards as what is at the end is most likely in the cpu cache.  it won't be much, but we'll take it. */
            for( set<WriteIntent>::const_iterator it(commitJob.writes().begin()), end(commitJob.writes().end()); it != end; ++it ) {
                const WriteIntent& intent = *it;
                stats.curr->_writeToDataFilesBytes += intent.length();
                dassert(intent.w_ptr);
                memcpy(intent.w_ptr,
                       commitJob._ab.atOfs(intent.ofsInJournalBuffer),
                       intent.length());
            }
        }
#endif

        /** apply the writes back to the non-private MMF after they are for certain in redo log

            (1) todo we don't need to write back everything every group commit.  we MUST write back
            that which is going to be a remapped on its private view - but that might not be all
            views.

            (2) todo should we do this using N threads?  would be quite easy
                see Hackenberg paper table 5 and 6.  2 threads might be a good balance.

            (3) with enough work, we could do this outside the read lock.  it's a bit tricky though.
                - we couldn't do it from the private views then as they may be changing.  would have to then
                  be from the journal alignedbuffer.
                - we need to be careful the file isn't unmapped on us -- perhaps a mutex or something
                  with MongoMMF on closes or something to coordinate that.

            concurrency: in mmmutex, not necessarily in dbMutex

            @see https://docs.google.com/drawings/edit?id=1TklsmZzm7ohIZkwgeK6rMvsdaR13KjtJYMsfLr175Zc&hl=en
        */

        void WRITETODATAFILES(const JSectHeader& h, AlignedBuilder& uncompressed) {
            Timer t;
            WRITETODATAFILES_Impl1(h, uncompressed);
            unsigned long long m = t.micros();
            stats.curr->_writeToDataFilesMicros += m;
            LOG(2) << "journal WRITETODATAFILES " << m / 1000.0 << "ms" << endl;
        }

    }
}
