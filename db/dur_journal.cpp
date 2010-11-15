// @file dur_journal.cpp

/**
*    Copyright (C) 2010 10gen Inc.
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

#if defined(_DURABLE)

#include "client.h"
#include "namespace.h"
#include "dur_journal.h"
#include "../util/logfile.h"
#include "../util/timer.h"
#include <boost/static_assert.hpp>
#undef assert
#define assert MONGO_assert
#include "../util/mongoutils/str.h"
#include "../util/concurrency/mvar.h"

namespace mongo {
    using namespace mongoutils;

    namespace dur {
        BOOST_STATIC_ASSERT( sizeof(JHeader) == 8192 );
        BOOST_STATIC_ASSERT( sizeof(JSectHeader) == 8 );
        BOOST_STATIC_ASSERT( sizeof(JSectFooter) == 20 );
        BOOST_STATIC_ASSERT( sizeof(JEntry) == 8 );

        void journalingFailure(const char *msg) { 
            /** todo:
                (1) don't log too much
                (2) make an indicator in the journal dir that something bad happened. 
                (2b) refuse to do a[ recovery startup if that is there without manual override.
            */ 
            log() << "journaling error " << msg << endl;
        }

        struct Journal {
            static const unsigned long long DataLimit = 1 * 1024 * 1024 * 1024;

            unsigned long long written;
            unsigned nextFileNumber;
            LogFile *lf;
            string dir;

            Journal()
            { 
                written = 0;
                nextFileNumber = 0;
                lf = 0; 
            }

            void open();
            void rotate();
            void journal(const BufBuilder& b);

            path getFilePathFor(int filenumber) const;
        };

        static Journal j;

        path Journal::getFilePathFor(int filenumber) const { 
            filesystem::path p(dir);
            p /= (str::stream() << "j._" << filenumber);
            return p;
        }

        /** assure journal/ dir exists. throws */
        void journalMakeDir() {
            filesystem::path p(dbpath);
            p /= "journal";
            j.dir = p.string();
            if( !exists(j.dir) ) {
                try {
                    create_directory(j.dir);
                }
                catch(std::exception& e) { 
                    log() << "error creating directory " << j.dir << ' ' << e.what() << endl;
                    throw;
                }
            }
        }

        /* threading: only durThread() calls this, thus safe. */
        void Journal::open() {
            assert( lf == 0 );
            string fname = getFilePathFor(nextFileNumber).string();
            lf = new LogFile(fname);
            nextFileNumber++;
            {
                JHeader h(fname);
                lf->synchronousAppend(&h, sizeof(h));
            }
        }

        static MVar<path> toUnlink;
        void unlinkThread() { 
            Client::initThread("unlink");
            while( 1 ) {
                path p = toUnlink.take();
                try {
                    remove(p);
                }
                catch(std::exception& e) { 
                    log() << "error unlink of journal file " << p.string() << " failed " << e.what() << endl;
                }
            }
        }

        /** check if time to rotate files.  assure a file is open. 
            done separately from the journal() call as we can do this part
            outside of lock.
         */
        void journalRotate() { 
            j.rotate();
        }
        void Journal::rotate() {
            if( lf && written < DataLimit ) 
                return;

            if( lf ) { 
                delete lf; // close
                lf = 0;
                written = 0;

                /* remove an older journal file. */
                if( nextFileNumber >= 3 ) {
                    unsigned fn = nextFileNumber - 3;
                    // we do unlinks asynchronously - unless they are falling behind.
                    // (unlinking big files can be slow on some operating systems; we don't want to stop world)
                    path p = j.getFilePathFor(fn);
                    if( !toUnlink.tryPut(p) ) {
                        /* DR___ for durability error and warning codes 
                           Compare to RS___ for replica sets
                        */
                        log() << "DR100 latency warning on journal unlink" << endl;
                        Timer t;
                        toUnlink.put(p);
                        log() << "toUnlink.put() " << t.millis() << "ms" << endl;
                    }
                }
            }

            try {
                Timer t;
                open();
                int ms = t.millis();
                if( ms >= 200 ) { 
                    log() << "DR101 latency warning on journal file open " << ms << "ms" << endl;
                }
            }
            catch(std::exception& e) { 
                log() << "warning exception in Journal::rotate" << e.what() << endl;
            }
        }

        /** write to journal             
        */
        void journal(const BufBuilder& b) {
            j.journal(b);
        }
        void Journal::journal(const BufBuilder& b) {
            try {
                /* todo: roll if too big */
                if( lf == 0 )
                    open();
                written += b.len();
                lf->synchronousAppend((void *) b.buf(), b.len());
            }
            catch(std::exception& e) { 
                log() << "warning exception in dur::journal " << e.what() << endl;
            }
        }

    }
}

#endif

/* todo 
   test (and handle) disk full on journal append 
*/
