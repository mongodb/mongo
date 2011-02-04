// @file dur_journal.cpp writing to the writeahead logging journal

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
#include "client.h"
#include "namespace.h"
#include "dur_journal.h"
#include "dur_journalformat.h"
#include "dur_stats.h"
#include "../util/logfile.h"
#include "../util/timer.h"
#include "../util/alignedbuilder.h"
#include "../util/message.h" // getelapsedtimemillis
#include "../util/concurrency/race.h"
#include <boost/static_assert.hpp>
#undef assert
#define assert MONGO_assert
#include "../util/mongoutils/str.h"
#include "dur_journalimpl.h"
#include "../util/file.h"

using namespace mongoutils;

namespace mongo {

    class AlignedBuilder;

    namespace dur {
        BOOST_STATIC_ASSERT( sizeof(JHeader) == 8192 );
        BOOST_STATIC_ASSERT( sizeof(JSectHeader) == 20 );
        BOOST_STATIC_ASSERT( sizeof(JSectFooter) == 32 );
        BOOST_STATIC_ASSERT( sizeof(JEntry) == 12 );
        BOOST_STATIC_ASSERT( sizeof(LSNFile) == 88 );

        void removeOldJournalFile(path p);

        filesystem::path getJournalDir() {
            filesystem::path p(dbpath);
            p /= "journal";
            return p;
        }

        path lsnPath() {
            return getJournalDir()/"lsn";
        }

        extern CodeBlock durThreadMain;

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
            _version = CurrentVersion;
            memset(ts, 0, sizeof(ts));
            time_t t = time(0);
            strncpy(ts, time_t_to_String_short(t).c_str(), sizeof(ts)-1);
            memset(dbpath, 0, sizeof(dbpath));
            strncpy(dbpath, fname.c_str(), sizeof(dbpath)-1);
            {
                fileId = t&0xffffffff;
                fileId |= ((unsigned long long)getRandomNumber()) << 32;
            }
            memset(reserved3, 0, sizeof(reserved3));
            txt2[0] = txt2[1] = '\n';
            n1 = n2 = n3 = n4 = '\n';
        }

        // class Journal

        Journal j;

        const unsigned long long LsnShutdownSentinel = ~((unsigned long long)0);

        Journal::Journal() :
            _curLogFileMutex("JournalLfMutex") {
            _written = 0;
            _nextFileNumber = 0;
            _curLogFile = 0;
            _curFileId = 0;
            _preFlushTime = 0;
            _lastFlushTime = 0;
            _writeToLSNNeeded = false;
        }

        path Journal::getFilePathFor(int filenumber) const {
            filesystem::path p(dir);
            p /= string(str::stream() << "j._" << filenumber);
            return p;
        }

        /** never throws
            @return true if journal dir is not empty
        */
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
            log() << "removeJournalFiles" << endl;
            try {
                for ( boost::filesystem::directory_iterator i( getJournalDir() );
                        i != boost::filesystem::directory_iterator();
                        ++i ) {
                    string fileName = boost::filesystem::path(*i).leaf();
                    if( str::startsWith(fileName, "j._") ) {
                        try {
                            removeOldJournalFile(*i);
                        }
                        catch(std::exception& e) {
                            log() << "couldn't remove " << fileName << ' ' << e.what() << endl;
                            throw;
                        }
                    }
                }
                try {
                    boost::filesystem::remove(lsnPath());
                }
                catch(...) {
                    log() << "couldn't remove " << lsnPath().string() << endl;
                    throw;
                }
            }
            catch( std::exception& e ) {
                log() << "error removing journal files " << e.what() << endl;
                throw;
            }
            assert(!haveJournalFiles());
            log(1) << "removeJournalFiles end" << endl;
        }

        /** at clean shutdown */
        bool okToCleanUp = false; // successful recovery would set this to true
        void Journal::cleanup() {
            if( !okToCleanUp )
                return;

            try {
                scoped_lock lk(_curLogFileMutex);
                closeCurrentJournalFile();
                removeJournalFiles();
            }
            catch(std::exception& e) {
                log() << "error couldn't remove journal file during shutdown " << e.what() << endl;
                throw;
            }
        }
        void journalCleanup() { j.cleanup(); }

        void preallocateFiles() {
            for( int i = 0; i <= 2; i++ ) {
                string fn = str::stream() << "prealloc." << i;
                filesystem::path filepath = getJournalDir() / fn;
                File f;
                f.open(filepath.string().c_str());
                uassert(13640, str::stream() << "couldn't open " << filepath.string(), f.is_open());
                if( f.len() )
                    continue;

                // preallocate
                log() << "preallocating a journal file " << filepath.string() << endl;
                const unsigned SZ = 1024 * 1024;
                unsigned long long limit = Journal::DataLimit;
                if( debug && i == 1 ) { 
                    // moving 32->64, the prealloc files would be short.  that is "ok", but we want to exercise that 
                    // case, so we force exercising here when _DEBUG is set by arbitrarily stopping prealloc at a low 
                    // limit for a file.  also we want to be able to change in the future the constant without a lot of
                    // work anyway.
                    limit = 16 * 1024 * 1024;
                }

#if defined(POSIX_FADV_DONTNEED)
		posix_fadvise(f.fd, 0, limit, POSIX_FADV_DONTNEED);
#endif

                scoped_ptr<char> data( new char[SZ] );
                for( fileofs o = 0; o < limit; o += SZ ) { 
                    f.write(o, data.get(), SZ);
                    uassert(13641, str::stream() << "error writing to " << filepath.string(), !f.bad());
                }

		// perhaps not necessary but will make the logging and behavior more readily understood
		f.fsync();
            }
        }

        void removeOldJournalFile(path p) { 
            try {
                for( int i = 0; i <= 2; i++ ) {
                    string fn = str::stream() << "prealloc." << i;
                    filesystem::path filepath = getJournalDir() / fn;
                    if( !filesystem::exists(filepath) ) {
                        // we can recycle this file into this prealloc file location
                        boost::filesystem::rename(p, filepath);
                        return;
                    }
                }
            } catch(...) { 
                log() << "warning exception in dur::removeOldJournalFile " << p.string() << endl;
                // fall through and try to delete the file
            }

            // already have 3 prealloc files, so delete this file
            try {
                boost::filesystem::remove(p);
            }
            catch(...) { 
                log() << "warning exception removing " << p.string() << endl;
            }
        }

        // find a prealloc.<n> file, presumably to take and use
        path findPrealloced() { 
            try {
                for( int i = 0; i <= 2; i++ ) {
                    string fn = str::stream() << "prealloc." << i;
                    filesystem::path filepath = getJournalDir() / fn;
                    if( filesystem::exists(filepath) )
                        return filepath;
                }
            } catch(...) { 
                log() << "warning exception in dur::findPrealloced()" << endl;
            }
            return path();
        }

        /** assure journal/ dir exists. throws. call during startup. */
        void journalMakeDir() {
            j.init();

            filesystem::path p = getJournalDir();
            j.dir = p.string();
            log() << "journal dir=" << j.dir << endl;
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
            _curFileId = 0;
            assert( _curLogFile == 0 );
            path fname = getFilePathFor(_nextFileNumber);

            // if we have a prealloced file, use it 
            {
                path p = findPrealloced();
                if( !p.empty() ) { 
                    try { 
                        {
                            // JHeader::fileId must be updated before renaming to be race-safe
                            LogFile f(p.string());
                            JHeader h(p.string());
                            AlignedBuilder b(8192);
                            b.appendStruct(h);
                            f.synchronousAppend(b.buf(), b.len());
                        }
                        boost::filesystem::rename(p, fname);
                    }
                    catch(...) { 
                        log() << "warning couldn't write to / rename file " << p.string() << endl;
                    }
                }
            }

            _curLogFile = new LogFile(fname.string());
            _nextFileNumber++;
            {
                JHeader h(fname.string());
                _curFileId = h.fileId;
                assert(_curFileId);
                AlignedBuilder b(8192);
                b.appendStruct(h);
                _curLogFile->synchronousAppend(b.buf(), b.len());
            }
        }

        void Journal::init() {
            assert( _curLogFile == 0 );
            MongoFile::notifyPreFlush = preFlush;
            MongoFile::notifyPostFlush = postFlush;
        }

        void Journal::open() {
            assert( MongoFile::notifyPreFlush == preFlush );
            mutex::scoped_lock lk(_curLogFileMutex);
            _open();
        }

        void LSNFile::set(unsigned long long x) {
            memset(this, 0, sizeof(*this));
            lsn = x;
            checkbytes = ~x;
        }

        /** logs details of the situation, and returns 0, if anything surprising in the LSNFile
            if something highly surprising, throws to abort
        */
        unsigned long long LSNFile::get() {
            uassert(13614, "unexpected version number of lsn file in journal/ directory", ver == 0);
            if( ~lsn != checkbytes ) {
                log() << "lsnfile not valid. recovery will be from log start. lsn: " << hex << lsn << " checkbytes: " << hex << checkbytes << endl;
                return 0;
            }
            return lsn;
        }

        /** called during recovery (the error message text below assumes that)
        */
        unsigned long long journalReadLSN() {
            if( !debug ) {
                // in nondebug build, for now, be conservative until more tests written, and apply the whole journal.
                // however we will still write the lsn file to exercise that code, and use in _DEBUG build.
                return 0;
            }

            if( !MemoryMappedFile::exists(lsnPath()) ) {
                log() << "info no lsn file in journal/ directory" << endl;
                return 0;
            }

            try {
                // os can flush as it likes.  if it flushes slowly, we will just do extra work on recovery.
                // however, given we actually close the file when writing, that seems unlikely.
                LSNFile L;
                File f;
                f.open(lsnPath().string().c_str());
                assert(f.is_open());
                f.read(0,(char*)&L, sizeof(L));
                unsigned long long lsn = L.get();
                return lsn;
            }
            catch(std::exception& e) {
                uasserted(13611, str::stream() << "can't read lsn file in journal directory : " << e.what());
            }
            return 0;
        }

        unsigned long long getLastDataFileFlushTime() {
            return j.lastFlushTime();
        }

        /** remember "last sequence number" to speed recoveries
            concurrency: called by durThread only.
        */
        void Journal::updateLSNFile() {
            if( !_writeToLSNNeeded )
                return;
            durThreadMain.assertWithin();
            _writeToLSNNeeded = false;
            try {
                // os can flush as it likes.  if it flushes slowly, we will just do extra work on recovery.
                // however, given we actually close the file, that seems unlikely.
                File f;
                f.open(lsnPath().string().c_str());
                if( !f.is_open() ) { 
                    // can get 0 if an i/o error
                    log() << "warning: open of lsn file failed" << endl;
                    return;
                }
                log() << "lsn set " << _lastFlushTime << endl;
                LSNFile lsnf;
                lsnf.set(_lastFlushTime);
                f.write(0, (char*)&lsnf, sizeof(lsnf));
            }
            catch(std::exception& e) {
                log() << "warning: write to lsn file failed " << e.what() << endl;
                // keep running (ignore the error). recovery will be slow.
            }
        }

        void Journal::preFlush() {
            j._preFlushTime = Listener::getElapsedTimeMillis();
        }

        void Journal::postFlush() {
            j._lastFlushTime = j._preFlushTime;
            j._writeToLSNNeeded = true;
        }

        // call from within _curLogFileMutex
        void Journal::closeCurrentJournalFile() {
            if (!_curLogFile)
                return;

            JFile jf;
            jf.filename = _curLogFile->_name;
            jf.lastEventTimeMs = Listener::getElapsedTimeMillis();
            _oldJournalFiles.push_back(jf);

            delete _curLogFile; // close
            _curLogFile = 0;
            _written = 0;
        }

        /** remove older journal files.
            be in _curLogFileMutex but not dbMutex when calling
        */
        void Journal::removeUnneededJournalFiles() {
            while( !_oldJournalFiles.empty() ) {
                JFile f = _oldJournalFiles.front();

                if( f.lastEventTimeMs < _lastFlushTime + ExtraKeepTimeMs ) {
                    // eligible for deletion
                    path p( f.filename );
                    log() << "old journal file will be removed: " << f.filename << endl;
                    removeOldJournalFile(p);
                }
                else {
                    break;
                }

                _oldJournalFiles.pop_front();
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
            assert( !dbMutex.atLeastReadLocked() );
            durThreadMain.assertWithin();

            scoped_lock lk(_curLogFileMutex);

            if ( inShutdown() )
                return;

            j.updateLSNFile();

            if( _curLogFile && _written < DataLimit )
                return;

            if( _curLogFile ) {

                closeCurrentJournalFile();

                removeUnneededJournalFiles();
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
                throw;
            }
        }

        /** write to journal
        */
        void journal(const AlignedBuilder& b) {
            j.journal(b);
        }
        void Journal::journal(const AlignedBuilder& b) {
            try {
                mutex::scoped_lock lk(_curLogFileMutex);
                if( _curLogFile == 0 )
                    _open();
                stats.curr->_journaledBytes += b.len();
                _written += b.len();
                _curLogFile->synchronousAppend((void *) b.buf(), b.len());
            }
            catch(std::exception& e) {
                log() << "warning exception in dur::journal " << e.what() << endl;
                throw;
            }
        }

    }
}

/* todo
   test (and handle) disk full on journal append.  best quick thing to do is to terminate.
   if we roll back operations, there are nuances such as is ReplSetImpl::lastOpTimeWritten too new in ram then?
*/
