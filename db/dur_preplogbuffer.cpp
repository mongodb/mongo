// @file dur_preplogbuffer.cpp 

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

/* 
     PREPLOGBUFFER 
       we will build an output buffer ourself and then use O_DIRECT
       we could be in read lock for this
       for very large objects write directly to redo log in situ?
     @see https://docs.google.com/drawings/edit?id=1TklsmZzm7ohIZkwgeK6rMvsdaR13KjtJYMsfLr175Zc
*/

#include "pch.h"
#include "cmdline.h"
#include "dur.h"
#include "dur_journal.h"
#include "dur_commitjob.h"
#include "../util/mongoutils/hash.h"
#include "../util/mongoutils/str.h"
#include "dur_stats.h"

using namespace mongoutils;

namespace mongo { 
    namespace dur {

        RelativePath local = RelativePath::fromRelativePath("local");

        MongoMMF* findMMF(void *ptr, size_t &ofs) {
            MongoMMF *f = privateViews._find(ptr, ofs);
            if( f == 0 ) {
                string s = str::stream() << "view pointer cannot be resolved " << (size_t) ptr;
                journalingFailure(s.c_str()); // asserts
            }
            return f;
        }

        /** we will build an output buffer ourself and then use O_DIRECT
            we could be in read lock for this
            caller handles locking
        */
        void PREPLOGBUFFER() { 
            assert( cmdLine.dur );
            AlignedBuilder& bb = commitJob._ab;
            bb.reset();

            unsigned lenOfs; // we will need to backfill the length when prep is wrapping up

            // JSectHeader
            {
                bb.appendStr("\nHH\n", false);
                lenOfs = bb.skip(4);
            }

            // ops other than basic writes (DurOp's)
            {
                for( vector< shared_ptr<DurOp> >::iterator i = commitJob.ops().begin(); i != commitJob.ops().end(); ++i ) { 
                    (*i)->serialize(bb);
                }
            }

            // basic write ops / write intents.  note there is no particular order to these : if we have 
            // two writes to the same location during the group commit interval, it is likely
            // (although not assured) that it is journaled here once.
            // 
            // "objAppend" operations are herein too
            //
            {
                scoped_lock lk(privateViews._mutex());

                // each time events switch to a different database we journal a JDbContext 
                RelativePath lastDbPath;

                for( vector<BasicWriteOp>::iterator i = commitJob.basicWrites().begin(); i != commitJob.basicWrites().end(); i++ ) {
                    size_t ofs = 1;
                    MongoMMF *mmf = findMMF(i->src, /*out*/ofs);

                    if( i->dst ) { 
                        // objAppend operation

                        if( mmf->relativePath() != lastDbPath ) { 
                            lastDbPath = mmf->relativePath();
                            dassert( lastDbPath != local ); // objAppend is not used for the local database
                            JDbContext c;
                            bb.appendStruct(c);
                            bb.appendStr(lastDbPath.toString());
                        }

                        size_t dstofs;
                        MongoMMF *dstmmf = findMMF(i->dst, dstofs);
                        if( !dstmmf->willNeedRemap() ) {
                            dstmmf->willNeedRemap() = true;
                        }

                        // since we have already looked up the mmf, we go ahead and remember the write view location 
                        // so we don't have to find the MongoMMF again later in WRITETODATAFILES()
                        i->dst = ((char*)dstmmf->view_write()) + dstofs;

                        JObjAppend d;
                        d.dstFileNo = dstmmf->fileSuffixNo();
                        d.dstOfs = (unsigned) dstofs;
                        d.srcFileNo = mmf->fileSuffixNo();
                        d.srcOfs = ofs;
                        d.len = i->len();
                        bb.appendStruct(d);
                        ++stats.curr._objCopies;
                    }
                    else {
                        if( !mmf->willNeedRemap() ) {
                            // tag this mmf as needed a remap of its private view later. 
                            // usually it will already be dirty/already set, so we do the if above first
                            // to avoid possibility of cpu cache line contention
                            mmf->willNeedRemap() = true;
                        }

                        // since we have already looked up the mmf, we go ahead and remember the write view location 
                        // so we don't have to find the MongoMMF again later in WRITETODATAFILES()
                        dassert( i->dst == 0 );
                        i->dst = ((char*)mmf->view_write()) + ofs;

                        JEntry e;
                        e.len = i->len();
                        assert( ofs <= 0x80000000 );
                        e.ofs = (unsigned) ofs;
                        e.setFileNo( mmf->fileSuffixNo() );
                        if( mmf->relativePath() == local ) { 
                            e.setLocalDbContextBit();
                        }
                        else if( mmf->relativePath() != lastDbPath ) { 
                            lastDbPath = mmf->relativePath();
                            JDbContext c;
                            bb.appendStruct(c);
                            bb.appendStr(lastDbPath.toString());
                        }
                        bb.appendStruct(e);
                        bb.appendBuf(i->src, i->len());
                    }
                }
            }

            {
                JSectFooter f(bb.buf(), bb.len());
                bb.appendStruct(f);
            }

            {
                // pad to alignment, and set the total section length in the JSectHeader
                assert( 0xffffe000 == (~(Alignment-1)) );
                unsigned L = (bb.len() + Alignment-1) & (~(Alignment-1));
                dassert( L >= (unsigned) bb.len() );
                *((unsigned*)bb.atOfs(lenOfs)) = L;
                unsigned padding = L - bb.len();
                bb.skip(padding);
                dassert( bb.len() % Alignment == 0 );
            }

            return;
        }

    }
}
