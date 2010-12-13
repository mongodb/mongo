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
#include "dur_journalformat.h"
#include "../util/logfile.h"
#include "../util/timer.h"
#include "../util/alignedbuilder.h"
#include <boost/static_assert.hpp>
#undef assert
#define assert MONGO_assert
#include "../util/mongoutils/str.h"
#include "../util/concurrency/mvar.h"

namespace mongo {
    using namespace mongoutils;

    class AlignedBuilder;

    namespace dur {
        BOOST_STATIC_ASSERT( sizeof(JHeader) == 8192 );
        BOOST_STATIC_ASSERT( sizeof(JSectHeader) == 8 );
        BOOST_STATIC_ASSERT( sizeof(JSectFooter) == 32 );
        BOOST_STATIC_ASSERT( sizeof(JEntry) == 12 );

        filesystem::path getJournalDir() { 
            filesystem::path p(dbpath);
            p /= "journal";
            return p;
        }

        /** this should be called when something really bad happens so that we can flag appropriately
        */
        void journalingFailure(const char *msg) { 
            /** todo:
                (1) don't log too much
                (2) make an indicator in the journal dir that something bad happened. 
                (2b) refuse to do a recovery startup if that is there without manual override.
            */ 
            log() << "journaling error " << msg << endl;
            assert(false);
        }

        JHeader::JHeader(string fname) { 
            magic[0] = 'j'; magic[1] = '\n';
            version = 0x4141;
            memset(ts, 0, sizeof(ts));
            strncpy(ts, time_t_to_String_short(time(0)).c_str(), sizeof(ts)-1);
            memset(dbpath, 0, sizeof(dbpath));
            strncpy(dbpath, fname.c_str(), sizeof(dbpath)-1);
            memset(reserved3, 0, sizeof(reserved3));
            txt2[0] = txt2[1] = '\n';
            n1 = n2 = n3 = n4 = '\n';
        }

        struct Journal {
            static const unsigned long long DataLimit = 1 * 1024 * 1024 * 1024;
        public:
            string dir; // set by journalMakeDir() during initialization
            MVar<path> &toUnlink;

            Journal() : 
              toUnlink(*(new MVar<path>)), /* freeing MVar at program termination would be problematic */
              _lfMutex("lfMutex")
            { 
                _written = 0;
                _nextFileNumber = 0;
                _lf = 0; 
            }

            void open();
            void rotate();
            void journal(const AlignedBuilder& b);

            path getFilePathFor(int filenumber) const;

            bool tryToCloseLogFile() { 
                mutex::try_lock lk(_lfMutex, 2000);
                if( lk.ok ) {
                    delete _lf; 
                    _lf = 0;
                }
                return lk.ok;
            }

        private:
            void _open();

            unsigned long long _written; // bytes written so far
            unsigned _nextFileNumber;

            LogFile *_lf;
            mutex _lfMutex; // lock when using _lf
        };

        static Journal j;

        path Journal::getFilePathFor(int filenumber) const { 
            filesystem::path p(dir);
            p /= (str::stream() << "j._" << filenumber);
            return p;
        }

        /** never throws */
        bool haveJournalFiles() { 
            try {
                for ( boost::filesystem::directory_iterator i( getJournalDir() );
                      i != boost::filesystem::directory_iterator(); 
                      ++i ) {
                    string fileName = boost::filesystem::path(*i).leaf();
                    if( str::startsWith(fileName, "j._") )
                        return true;
                }
            }
            catch(...) { }
            return false;
        }
        
        /** throws */
        void removeJournalFiles() { 
            try {
                for ( boost::filesystem::directory_iterator i( getJournalDir() );
                      i != boost::filesystem::directory_iterator(); 
                      ++i ) {
                    string fileName = boost::filesystem::path(*i).leaf();
                    if( str::startsWith(fileName, "j._") ) {
                        try {
                            boost::filesystem::remove(*i);
                        }
                        catch(std::exception& e) {
                            log() << "couldn't remove " << fileName << ' ' << e.what() << endl;
                        }
                    }
                }
            }
            catch( std::exception& e ) { 
                log() << "error removing journal files " << e.what() << endl;
                throw;
            }
        }

        /** at clean shutdown */
        bool okToCleanUp = false; // failed recovery would set this to false
        void journalCleanup() { 
            if( testIntent ) 
                return;
            if( !okToCleanUp ) 
                return;
            if( !j.tryToCloseLogFile() ) {
                return;
            }
            try { 
                removeJournalFiles(); 
            }
            catch(std::exception& e) {
                log() << "error couldn't remove journal file during shutdown " << e.what() << endl;
            }
        }

        /** assure journal/ dir exists. throws */
        void journalMakeDir() {
            filesystem::path p = getJournalDir();
            j.dir = p.string();
            DEV log() << "dev journalMakeDir() " << j.dir << endl;
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

        void Journal::_open() {
            assert( _lf == 0 );
            string fname = getFilePathFor(_nextFileNumber).string();
            _lf = new LogFile(fname);
            _nextFileNumber++;
            {
                JHeader h(fname);
                AlignedBuilder b(8192);
                b.appendStruct(h);
                _lf->synchronousAppend(b.buf(), b.len());
            }
        }

        void Journal::open() {
            mutex::scoped_lock lk(_lfMutex);
            _open();
        }

        void unlinkThread() { 
            Client::initThread("unlink");
            while( 1 ) {
                path p = j.toUnlink.take();
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
            thread: durThread()
         */
        void journalRotate() { 
            j.rotate();
        }
        void Journal::rotate() {
            if( _lf && _written < DataLimit ) 
                return;
            scoped_lock lk(_lfMutex);
            if( _lf && _written < DataLimit ) 
                return;

            if( _lf ) { 
                delete _lf; // close
                _lf = 0;
                _written = 0;

                /* remove an older journal file. */
                if( _nextFileNumber >= 3 ) {
                    unsigned fn = _nextFileNumber - 3;
                    // we do unlinks asynchronously - unless they are falling behind.
                    // (unlinking big files can be slow on some operating systems; we don't want to stop world)
                    path p = j.getFilePathFor(fn);
                    if( !j.toUnlink.tryPut(p) ) {
                        /* DR___ for durability error and warning codes 
                           Compare to RS___ for replica sets
                        */
                        log() << "DR100 latency warning on journal unlink" << endl;
                        Timer t;
                        j.toUnlink.put(p);
                        log() << "toUnlink.put() " << t.millis() << "ms" << endl;
                    }
                }
            }

            try {
                Timer t;
                _open();
                int ms = t.millis();
                if( ms >= 200 ) { 
                    log() << "DR101 latency warning on journal file open " << ms << "ms" << endl;
                }
            }
            catch(std::exception& e) { 
                log() << "warning exception opening journal file " << e.what() << endl;
            }
        }

        /** write to journal
            thread: durThread()
        */
        void journal(const AlignedBuilder& b) {
            j.journal(b);
        }
        void Journal::journal(const AlignedBuilder& b) {
            try {
                mutex::scoped_lock lk(_lfMutex);
                if( _lf == 0 )
                    open();
                _written += b.len();
                _lf->synchronousAppend((void *) b.buf(), b.len());
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
