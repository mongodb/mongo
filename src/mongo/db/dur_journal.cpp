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
#include "../util/net/listen.h" // getelapsedtimemillis
#include <boost/static_assert.hpp>
#include <boost/filesystem.hpp>
#include "../util/mongoutils/str.h"
#include "dur_journalimpl.h"
#include "../util/file.h"
#include "../util/checksum.h"
#include "../util/concurrency/race.h"
#include "../util/compress.h"
#include "../util/progress_meter.h"
#include "../server.h"
#include "../util/mmap.h"

using namespace mongoutils;

#include <boost/filesystem/operations.hpp>

namespace mongo {

    class AlignedBuilder;

    unsigned goodRandomNumberSlow();

    namespace dur {
        // Rotate after reaching this data size in a journal (j._<n>) file
        // We use a smaller size for 32 bit as the journal is mmapped during recovery (only)
        // Note if you take a set of datafiles, including journal files, from 32->64 or vice-versa, it must 
        // work.  (and should as-is)
        // --smallfiles makes the limit small.

#if defined(_DEBUG)
        unsigned long long DataLimitPerJournalFile = 128 * 1024 * 1024;
#elif defined(__APPLE__)
        // assuming a developer box if OS X
        unsigned long long DataLimitPerJournalFile = 256 * 1024 * 1024;
#else
        unsigned long long DataLimitPerJournalFile = (sizeof(void*)==4) ? 256 * 1024 * 1024 : 1 * 1024 * 1024 * 1024;
#endif

        BOOST_STATIC_ASSERT( sizeof(Checksum) == 16 );
        BOOST_STATIC_ASSERT( sizeof(JHeader) == 8192 );
        BOOST_STATIC_ASSERT( sizeof(JSectHeader) == 20 );
        BOOST_STATIC_ASSERT( sizeof(JSectFooter) == 32 );
        BOOST_STATIC_ASSERT( sizeof(JEntry) == 12 );
        BOOST_STATIC_ASSERT( sizeof(LSNFile) == 88 );

        bool usingPreallocate = false;

        void removeOldJournalFile(boost::filesystem::path p);

        boost::filesystem::path getJournalDir() {
            boost::filesystem::path p(dbpath);
            p /= "journal";
            return p;
        }

        boost::filesystem::path lsnPath() {
            return getJournalDir()/"lsn";
        }

        /** this should be called when something really bad happens so that we can flag appropriately
        */
        void journalingFailure(const char *msg) {
            /** todo:
                (1) don't log too much
                (2) make an indicator in the journal dir that something bad happened.
                (2b) refuse to do a recovery startup if that is there without manual override.
            */
            log() << "journaling failure/error: " << msg << endl;
            verify(false);
        }

        JSectFooter::JSectFooter() { 
            memset(this, 0, sizeof(*this));
            sentinel = JEntry::OpCode_Footer;
        }

        JSectFooter::JSectFooter(const void* begin, int len) { // needs buffer to compute hash
            sentinel = JEntry::OpCode_Footer;
            reserved = 0;
            magic[0] = magic[1] = magic[2] = magic[3] = '\n';

            Checksum c;
            c.gen(begin, (unsigned) len);
            memcpy(hash, c.bytes, sizeof(hash));
        }

        bool JSectFooter::checkHash(const void* begin, int len) const {
            if( !magicOk() ) { 
                log() << "journal footer not valid" << endl;
                return false;
            }
            Checksum c;
            c.gen(begin, len);
            DEV log() << "checkHash len:" << len << " hash:" << toHex(hash, 16) << " current:" << toHex(c.bytes, 16) << endl;
            if( memcmp(hash, c.bytes, sizeof(hash)) == 0 ) 
                return true;
            log() << "journal checkHash mismatch, got: " << toHex(c.bytes, 16) << " expected: " << toHex(hash,16) << endl;
            return false;
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
                fileId |= ((unsigned long long)goodRandomNumberSlow()) << 32;
            }
            memset(reserved3, 0, sizeof(reserved3));
            txt2[0] = txt2[1] = '\n';
            n1 = n2 = n3 = n4 = '\n';
        }

        Journal j;

        const unsigned long long LsnShutdownSentinel = ~((unsigned long long)0);

        Journal::Journal() :
            _curLogFileMutex("JournalLfMutex") {
            _ageOut = true;
            _written = 0;
            _nextFileNumber = 0;
            _curLogFile = 0;
            _curFileId = 0;
            _preFlushTime = 0;
            _lastFlushTime = 0;
            _writeToLSNNeeded = false;
        }

        boost::filesystem::path Journal::getFilePathFor(int filenumber) const {
            boost::filesystem::path p(dir);
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
            verify(!haveJournalFiles());

            flushMyDirectory(getJournalDir() / "file"); // flushes parent of argument (in this case journal dir)

            log(1) << "removeJournalFiles end" << endl;
        }

        /** at clean shutdown */
        bool okToCleanUp = false; // successful recovery would set this to true
        void Journal::cleanup(bool _log) {
            if( !okToCleanUp )
                return;

            if( _log )
                log() << "journalCleanup..." << endl;
            try {
                SimpleMutex::scoped_lock lk(_curLogFileMutex);
                closeCurrentJournalFile();
                removeJournalFiles();
            }
            catch(std::exception& e) {
                log() << "error couldn't remove journal file during shutdown " << e.what() << endl;
                throw;
            }
        }
        void journalCleanup(bool log) { j.cleanup(log); }

        bool _preallocateIsFaster() {
            bool faster = false;
            boost::filesystem::path p = getJournalDir() / "tempLatencyTest";
            try { remove(p); } catch(...) { }
            try {
                AlignedBuilder b(8192);
                int millis[2];
                const int N = 50;
                for( int pass = 0; pass < 2; pass++ ) {
                    LogFile f(p.string());
                    Timer t;
                    for( int i = 0 ; i < N; i++ ) { 
                        f.synchronousAppend(b.buf(), 8192);
                    }
                    millis[pass] = t.millis();
                    // second time through, file exists and is prealloc case
                }
                int diff = millis[0] - millis[1];
                if( diff > 2 * N ) {
                    // at least 2ms faster for prealloc case?
                    faster = true;
                    log() << "preallocateIsFaster=true " << diff / (1.0*N) << endl;
                }
            }
            catch(...) {
                log() << "info preallocateIsFaster couldn't run; returning false" << endl;
            }
            try { remove(p); } catch(...) { }
            return faster;
        }
        bool preallocateIsFaster() {
            Timer t;
            bool res = false;
            if( _preallocateIsFaster() && _preallocateIsFaster() ) { 
                // maybe system is just super busy at the moment? sleep a second to let it calm down.  
                // deciding to to prealloc is a medium big decision:
                sleepsecs(1);
                res = _preallocateIsFaster();
            }
            if( t.millis() > 3000 ) 
                log() << "preallocateIsFaster check took " << t.millis()/1000.0 << " secs" << endl;
            return res;
        }

        // throws
        void preallocateFile(boost::filesystem::path p, unsigned long long len) {
            if( exists(p) ) 
                return;
            
            log() << "preallocating a journal file " << p.string() << endl;

            const unsigned BLKSZ = 1024 * 1024;
            verify( len % BLKSZ == 0 );

            AlignedBuilder b(BLKSZ);            
            memset((void*)b.buf(), 0, BLKSZ);

            ProgressMeter m(len, 3/*secs*/, 10/*hits between time check (once every 6.4MB)*/);

            File f;
            f.open( p.string().c_str() , /*read-only*/false , /*direct-io*/false );
            verify( f.is_open() );
            fileofs loc = 0;
            while ( loc < len ) {
                f.write( loc , b.buf() , BLKSZ );
                loc += BLKSZ;
                m.hit(BLKSZ);
            }
            verify( loc == len );
            f.fsync();
        }

        const int NUM_PREALLOC_FILES = 3;
        inline boost::filesystem::path preallocPath(int n) {
            verify(n >= 0);
            verify(n < NUM_PREALLOC_FILES);
            string fn = str::stream() << "prealloc." << n;
            return getJournalDir() / fn;
        }

        // throws
        void _preallocateFiles() {
            for( int i = 0; i < NUM_PREALLOC_FILES; i++ ) {
                boost::filesystem::path filepath = preallocPath(i);

                unsigned long long limit = DataLimitPerJournalFile;
                if( debug && i == 1 ) { 
                    // moving 32->64, the prealloc files would be short.  that is "ok", but we want to exercise that 
                    // case, so we force exercising here when _DEBUG is set by arbitrarily stopping prealloc at a low 
                    // limit for a file.  also we want to be able to change in the future the constant without a lot of
                    // work anyway.
                    limit = 16 * 1024 * 1024;
                }
                preallocateFile(filepath, limit);
            }
        }

        void checkFreeSpace() {
            unsigned long long spaceNeeded = static_cast<unsigned long long>(3 * DataLimitPerJournalFile * 1.1); // add 10% for headroom
            unsigned long long freeSpace = File::freeSpace(getJournalDir().string());
            unsigned long long prealloced = 0;
            for( int i = 0; i < NUM_PREALLOC_FILES; i++ ) {
                boost::filesystem::path filepath = preallocPath(i);
                if (exists(filepath))
                    prealloced += file_size(filepath);
            }

            if (freeSpace + prealloced < spaceNeeded) {
                log() << endl;
                error() << "Insufficient free space for journal files" << endl;
                log() << "Please make at least " << spaceNeeded/(1024*1024) << "MB available in " << getJournalDir().string() << " or use --smallfiles" << endl;
                log() << endl;
                throw UserException(15926, "Insufficient free space for journals");
            }
        }

        void preallocateFiles() {
            if (! (cmdLine.durOptions & CmdLine::DurNoCheckSpace))
                checkFreeSpace();

            if( exists(preallocPath(0)) || // if enabled previously, keep using
                exists(preallocPath(1)) ||
                ( cmdLine.preallocj && preallocateIsFaster() ) ) {
                    usingPreallocate = true;
                    try {
                        _preallocateFiles();
                    }
                    catch(...) {
                        log() << "warning caught exception in preallocateFiles, continuing" << endl;
                    }
            }
            j.open();
        }

        void removeOldJournalFile(boost::filesystem::path p) { 
            if( usingPreallocate ) {
                try {
                    for( int i = 0; i < NUM_PREALLOC_FILES; i++ ) {
                        boost::filesystem::path filepath = preallocPath(i);
                        if( !boost::filesystem::exists(filepath) ) {
                            // we can recycle this file into this prealloc file location
                            boost::filesystem::path temppath = filepath.string() + ".temp";
                            boost::filesystem::rename(p, temppath);
                            {
                                // zero the header
                                File f;
                                f.open(temppath.string().c_str(), false, false);
                                char buf[8192];
                                memset(buf, 0, 8192);
                                f.write(0, buf, 8192);
                                f.truncate(DataLimitPerJournalFile);
                                f.fsync();
                            }
                            boost::filesystem::rename(temppath, filepath);
                            return;
                        }
                    }
                } catch(...) { 
                    log() << "warning exception in dur::removeOldJournalFile " << p.string() << endl;
                    // fall through and try to delete the file
                }
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
        boost::filesystem::path findPrealloced() { 
            try {
                for( int i = 0; i < NUM_PREALLOC_FILES; i++ ) {
                    boost::filesystem::path filepath = preallocPath(i);
                    if( boost::filesystem::exists(filepath) )
                        return filepath;
                }
            } catch(...) { 
                log() << "warning exception in dur::findPrealloced()" << endl;
            }
            return boost::filesystem::path();
        }

        /** assure journal/ dir exists. throws. call during startup. */
        void journalMakeDir() {
            j.init();

            boost::filesystem::path p = getJournalDir();
            j.dir = p.string();
            log() << "journal dir=" << j.dir << endl;
            if( !boost::filesystem::exists(j.dir) ) {
                try {
                    boost::filesystem::create_directory(j.dir);
                }
                catch(std::exception& e) {
                    log() << "error creating directory " << j.dir << ' ' << e.what() << endl;
                    throw;
                }
            }
        }

        void Journal::_open() {
            _curFileId = 0;
            verify( _curLogFile == 0 );
            boost::filesystem::path fname = getFilePathFor(_nextFileNumber);

            // if we have a prealloced file, use it 
            {
                boost::filesystem::path p = findPrealloced();
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
                verify(_curFileId);
                AlignedBuilder b(8192);
                b.appendStruct(h);
                _curLogFile->synchronousAppend(b.buf(), b.len());
            }
        }

        void Journal::init() {
            verify( _curLogFile == 0 );
            MongoFile::notifyPreFlush = preFlush;
            MongoFile::notifyPostFlush = postFlush;
        }

        void Journal::open() {
            verify( MongoFile::notifyPreFlush == preFlush );
            SimpleMutex::scoped_lock lk(_curLogFileMutex);
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
            uassert(13614, str::stream() << "unexpected version number of lsn file in journal/ directory got: " << ver , ver == 0);
            if( ~lsn != checkbytes ) {
                log() << "lsnfile not valid. recovery will be from log start. lsn: " << hex << lsn << " checkbytes: " << hex << checkbytes << endl;
                return 0;
            }
            return lsn;
        }

        /** called during recovery (the error message text below assumes that)
        */
        unsigned long long journalReadLSN() {
            if( !exists(lsnPath()) ) {
                log() << "info no lsn file in journal/ directory" << endl;
                return 0;
            }

            try {
                // os can flush as it likes.  if it flushes slowly, we will just do extra work on recovery.
                // however, given we actually close the file when writing, that seems unlikely.
                LSNFile L;
                File f;
                f.open(lsnPath().string().c_str());
                verify(f.is_open());
                if( f.len() == 0 ) { 
                    // this could be 'normal' if we crashed at the right moment
                    log() << "info lsn file is zero bytes long" << endl;
                    return 0;
                }
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
            RACECHECK
            if( !_writeToLSNNeeded )
                return;
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
                LOG(1) << "lsn set " << _lastFlushTime << endl;
                LSNFile lsnf;
                lsnf.set(_lastFlushTime);
                f.write(0, (char*)&lsnf, sizeof(lsnf));
				// do we want to fsync here? if we do it probably needs to be async so the durthread
				// is not delayed.
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
                    boost::filesystem::path p( f.filename );
                    log() << "old journal file will be removed: " << f.filename << endl;
                    removeOldJournalFile(p);
                }
                else {
                    break;
                }

                _oldJournalFiles.pop_front();
            }
        }

        /*int getAgeOutJournalFiles() {
            mutex::try_lock lk(j._curLogFileMutex, 4000);
            if( !lk.ok )
                return -1;
            return j._ageOut ? 1 : 0;
        }*/
        void setAgeOutJournalFiles(bool a) {
            SimpleMutex::scoped_lock lk(j._curLogFileMutex);
            j._ageOut = a;
        }

        void Journal::_rotate() {
            if ( Lock::isLocked() ) {
                LOGSOME << "info journal _rotate called inside dbMutex - ok but should be somewhat rare" << endl;
            }

            RACECHECK;

            _curLogFileMutex.dassertLocked();

            if ( inShutdown() || !_curLogFile )
                return;

            j.updateLSNFile();

            if( _curLogFile && _written < DataLimitPerJournalFile )
                return;

            if( _curLogFile ) {
                _curLogFile->truncate();
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

        /** write (append) the buffer we have built to the journal and fsync it.
            outside of dbMutex lock as this could be slow.
            @param uncompressed - a buffer that will be written to the journal after compression
            will not return until on disk
        */
        void WRITETOJOURNAL(JSectHeader h, AlignedBuilder& uncompressed) {
            Timer t;
            j.journal(h, uncompressed);
            stats.curr->_writeToJournalMicros += t.micros();
        }
        void Journal::journal(const JSectHeader& h, const AlignedBuilder& uncompressed) {
            RACECHECK
            static AlignedBuilder b(32*1024*1024);
            /* buffer to journal will be
               JSectHeader
               compressed operations
               JSectFooter
            */
            const unsigned headTailSize = sizeof(JSectHeader) + sizeof(JSectFooter);
            const unsigned max = maxCompressedLength(uncompressed.len()) + headTailSize;
            b.reset(max);

            {
                dassert( h.sectionLen() == (unsigned) 0xffffffff ); // we will backfill later
                b.appendStruct(h);
            }

            size_t compressedLength = 0;
            rawCompress(uncompressed.buf(), uncompressed.len(), b.cur(), &compressedLength);
            verify( compressedLength < 0xffffffff );
            verify( compressedLength < max );
            b.skip(compressedLength);

            // footer
            unsigned L = 0xffffffff;
            {
                // pad to alignment, and set the total section length in the JSectHeader
                verify( 0xffffe000 == (~(Alignment-1)) );
                unsigned lenUnpadded = b.len() + sizeof(JSectFooter);
                L = (lenUnpadded + Alignment-1) & (~(Alignment-1));
                dassert( L >= lenUnpadded );

                ((JSectHeader*)b.atOfs(0))->setSectionLen(lenUnpadded);

                JSectFooter f(b.buf(), b.len()); // computes checksum
                b.appendStruct(f);
                dassert( b.len() == lenUnpadded );

                b.skip(L - lenUnpadded);
                dassert( b.len() % Alignment == 0 );
            }

            try {
                SimpleMutex::scoped_lock lk(_curLogFileMutex);

                // must already be open -- so that _curFileId is correct for previous buffer building
                verify( _curLogFile );

                stats.curr->_uncompressedBytes += uncompressed.len();
                unsigned w = b.len();
                _written += w;
                verify( w <= L );
                stats.curr->_journaledBytes += L;
                _curLogFile->synchronousAppend((const void *) b.buf(), L);
                _rotate();
            }
            catch(std::exception& e) {
                log() << "error exception in dur::journal " << e.what() << endl;
                throw;
            }
        }

    }
}

/* todo
   test (and handle) disk full on journal append.  best quick thing to do is to terminate.
   if we roll back operations, there are nuances such as is ReplSetImpl::lastOpTimeWritten too new in ram then?
*/
