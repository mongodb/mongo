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

/*
     PREPLOGBUFFER
       we will build an output buffer ourself and then use O_DIRECT
       we could be in read lock for this
       for very large objects write directly to redo log in situ?
     @see https://docs.google.com/drawings/edit?id=1TklsmZzm7ohIZkwgeK6rMvsdaR13KjtJYMsfLr175Zc
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/aligned_builder.h"
#include "mongo/db/storage/mmap_v1/dur_commitjob.h"
#include "mongo/db/storage/mmap_v1/dur_journal.h"
#include "mongo/db/storage/mmap_v1/dur_journalimpl.h"
#include "mongo/db/storage/mmap_v1/dur_stats.h"
#include "mongo/db/storage/mmap_v1/durable_mapped_file.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::endl;
using std::min;
using std::stringstream;

namespace dur {

extern Journal j;
extern CommitJob commitJob;

const RelativePath local = RelativePath::fromRelativePath("local");

static DurableMappedFile* findMMF_inlock(void* ptr, size_t& ofs) {
    DurableMappedFile* f = privateViews.find_inlock(ptr, ofs);
    if (f == 0) {
        error() << "findMMF_inlock failed " << privateViews.numberOfViews_inlock() << endl;

        // we want a stack trace and the assert below didn't print a trace once in the real world
        // - not sure why
        printStackTrace();
        stringstream ss;
        ss << "view pointer cannot be resolved " << std::hex << (size_t)ptr;
        journalingFailure(ss.str().c_str());  // asserts, which then abends
    }
    return f;
}

/** put the basic write operation into the buffer (bb) to be journaled */
static void prepBasicWrite_inlock(AlignedBuilder& bb,
                                  const WriteIntent* i,
                                  RelativePath& lastDbPath) {
    size_t ofs = 1;
    DurableMappedFile* mmf = findMMF_inlock(i->start(), /*out*/ ofs);

    if (MONGO_unlikely(!mmf->willNeedRemap())) {
        // tag this mmf as needed a remap of its private view later.
        // usually it will already be dirty/already set, so we do the if above first
        // to avoid possibility of cpu cache line contention
        mmf->setWillNeedRemap();
    }

    // since we have already looked up the mmf, we go ahead and remember the write view location
    // so we don't have to find the DurableMappedFile again later in WRITETODATAFILES()
    //
    // this was for WRITETODATAFILES_Impl2 so commented out now
    //
    /*
    dassert( i->w_ptr == 0 );
    i->w_ptr = ((char*)mmf->view_write()) + ofs;
    */

    JEntry e;
    e.len = min(i->length(), (unsigned)(mmf->length() - ofs));  // don't write past end of file
    verify(ofs <= 0x80000000);
    e.ofs = (unsigned)ofs;
    e.setFileNo(mmf->fileSuffixNo());

    if (mmf->relativePath() == local) {
        e.setLocalDbContextBit();
    } else if (mmf->relativePath() != lastDbPath) {
        lastDbPath = mmf->relativePath();
        JDbContext c;
        bb.appendStruct(c);
        bb.appendStr(lastDbPath.toString());
    }

    bb.appendStruct(e);
    bb.appendBuf(i->start(), e.len);

    if (MONGO_unlikely(e.len != (unsigned)i->length())) {
        log() << "journal info splitting prepBasicWrite at boundary" << endl;

        // This only happens if we write to the last byte in a file and
        // the fist byte in another file that is mapped adjacently. I
        // think most OSs leave at least a one page gap between
        // mappings, but better to be safe.

        WriteIntent next((char*)i->start() + e.len, i->length() - e.len);
        prepBasicWrite_inlock(bb, &next, lastDbPath);
    }
}

/** basic write ops / write intents.  note there is no particular order to these : if we have
    two writes to the same location during the group commit interval, it is likely
    (although not assured) that it is journaled here once.
*/
static void prepBasicWrites(AlignedBuilder& bb, const std::vector<WriteIntent>& intents) {
    stdx::lock_guard<stdx::mutex> lk(privateViews._mutex());

    // Each time write intents switch to a different database we journal a JDbContext.
    // Switches will be rare as we sort by memory location first and we batch commit.
    RelativePath lastDbPath;

    invariant(!intents.empty());

    WriteIntent last;
    for (std::vector<WriteIntent>::const_iterator i = intents.begin(); i != intents.end(); i++) {
        if (i->start() < last.end()) {
            // overlaps
            last.absorb(*i);
        } else {
            // discontinuous
            if (i != intents.begin()) {
                prepBasicWrite_inlock(bb, &last, lastDbPath);
            }

            last = *i;
        }
    }

    prepBasicWrite_inlock(bb, &last, lastDbPath);
}

/** we will build an output buffer ourself and then use O_DIRECT
    we could be in read lock for this
    caller handles locking
    @return partially populated sectheader and _ab set
*/
static void _PREPLOGBUFFER(JSectHeader& h,
                           AlignedBuilder& bb,
                           ClockSource* cs,
                           int64_t serverStartMs) {
    // Add the JSectHeader

    // Invalidate the total length, we will fill it in later.
    h.setSectionLen(0xffffffff);
    h.seqNumber = generateNextSeqNumber(cs, serverStartMs);
    h.fileId = j.curFileId();

    // Ops other than basic writes (DurOp's) go first
    const std::vector<std::shared_ptr<DurOp>>& durOps = commitJob.ops();
    for (std::vector<std::shared_ptr<DurOp>>::const_iterator i = durOps.begin(); i != durOps.end();
         i++) {
        (*i)->serialize(bb);
    }

    // Write intents
    const std::vector<WriteIntent>& intents = commitJob.getIntentsSorted();
    if (!intents.empty()) {
        prepBasicWrites(bb, intents);
    }
}

void PREPLOGBUFFER(/*out*/ JSectHeader& outHeader,
                   AlignedBuilder& outBuffer,
                   ClockSource* cs,
                   int64_t serverStartMs) {
    Timer t;
    j.assureLogFileOpen();  // so fileId is set
    _PREPLOGBUFFER(outHeader, outBuffer, cs, serverStartMs);
    stats.curr()->_prepLogBufferMicros += t.micros();
}
}
}
