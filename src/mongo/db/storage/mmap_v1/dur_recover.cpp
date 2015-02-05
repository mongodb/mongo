// @file dur_recover.cpp crash recovery via the journal

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kJournal

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/dur_recover.h"

#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sys/stat.h>

#include "mongo/db/operation_context_impl.h"
#include "mongo/db/storage/mmap_v1/dur_commitjob.h"
#include "mongo/db/storage/mmap_v1/dur_journal.h"
#include "mongo/db/storage/mmap_v1/dur_journalformat.h"
#include "mongo/db/storage/mmap_v1/dur_stats.h"
#include "mongo/db/storage/mmap_v1/durop.h"
#include "mongo/db/storage/mmap_v1/durable_mapped_file.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/checksum.h"
#include "mongo/util/compress.h"
#include "mongo/util/exit.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/startup_test.h"

namespace mongo {

    using boost::shared_ptr;
    using std::auto_ptr;
    using std::endl;
    using std::hex;
    using std::map;
    using std::pair;
    using std::setw;
    using std::string;
    using std::stringstream;
    using std::vector;

    /**
     * Thrown when a journal section is corrupt. This is considered OK as long as it occurs while
     * processing the last file. Processing stops at the first corrupt section.
     *
     * Any logging about the nature of the corruption should happen before throwing as this class
     * contains no data.
     */
    class JournalSectionCorruptException {};

    namespace dur {

        // The singleton recovery job object
        RecoveryJob& RecoveryJob::_instance = *(new RecoveryJob());


        void removeJournalFiles();
        boost::filesystem::path getJournalDir();


        struct ParsedJournalEntry { /*copyable*/
            ParsedJournalEntry() : e(0) { }

            // relative path of database for the operation.
            // might be a pointer into mmaped Journal file
            const char *dbName;

            // those are pointers into the memory mapped journal file
            const JEntry *e;  // local db sentinel is already parsed out here into dbName

            // if not one of the two simple JEntry's above, this is the operation:
            boost::shared_ptr<DurOp> op;
        };


        /**
         * Get journal filenames, in order. Throws if unexpected content found.
         */
        static void getFiles(boost::filesystem::path dir, vector<boost::filesystem::path>& files) {
            map<unsigned,boost::filesystem::path> m;
            for ( boost::filesystem::directory_iterator i( dir );
                    i != boost::filesystem::directory_iterator();
                    ++i ) {
                boost::filesystem::path filepath = *i;
                string fileName = boost::filesystem::path(*i).leaf().string();
                if( str::startsWith(fileName, "j._") ) {
                    unsigned u = str::toUnsigned( str::after(fileName, '_') );
                    if( m.count(u) ) {
                        uasserted(13531, str::stream() << "unexpected files in journal directory " << dir.string() << " : " << fileName);
                    }
                    m.insert( pair<unsigned,boost::filesystem::path>(u,filepath) );
                }
            }
            for( map<unsigned,boost::filesystem::path>::iterator i = m.begin(); i != m.end(); ++i ) {
                if( i != m.begin() && m.count(i->first - 1) == 0 ) {
                    uasserted(13532,
                    str::stream() << "unexpected file in journal directory " << dir.string()
                      << " : " << boost::filesystem::path(i->second).leaf().string() << " : can't find its preceding file");
                }
                files.push_back(i->second);
            }
        }

        /** read through the memory mapped data of a journal file (journal/j._<n> file)
            throws
        */
        class JournalSectionIterator {
            MONGO_DISALLOW_COPYING(JournalSectionIterator);
        public:
            JournalSectionIterator(const JSectHeader& h,
                                   const void *compressed,
                                   unsigned compressedLen,
                                   bool doDurOpsRecovering)
                : _h(h),
                  _lastDbName(0),
                  _doDurOps(doDurOpsRecovering) {

                verify(doDurOpsRecovering);

                if (!uncompress((const char *)compressed, compressedLen, &_uncompressed)) {
                    // We check the checksum before we uncompress, but this may still fail as the
                    // checksum isn't foolproof.
                    log() << "couldn't uncompress journal section" << endl;
                    throw JournalSectionCorruptException();
                }

                const char *p = _uncompressed.c_str();
                verify(compressedLen == _h.sectionLen() - sizeof(JSectFooter) - sizeof(JSectHeader));

                _entries = auto_ptr<BufReader>(new BufReader(p, _uncompressed.size()));
            }

            // We work with the uncompressed buffer when doing a WRITETODATAFILES (for speed)
            JournalSectionIterator(const JSectHeader &h, const void *p, unsigned len)
                : _entries(new BufReader((const char *)p, len)),
                  _h(h),
                  _lastDbName(0),
                  _doDurOps(false) {

            }

            bool atEof() const { return _entries->atEof(); }

            unsigned long long seqNumber() const { return _h.seqNumber; }

            /** get the next entry from the log.  this function parses and combines JDbContext and JEntry's.
             *  throws on premature end of section.
             */
            void next(ParsedJournalEntry& e) {
                unsigned lenOrOpCode;
                _entries->read(lenOrOpCode);

                if (lenOrOpCode > JEntry::OpCode_Min) {
                    switch( lenOrOpCode ) {

                    case JEntry::OpCode_Footer: {
                        verify( false );
                    }

                    case JEntry::OpCode_FileCreated:
                    case JEntry::OpCode_DropDb: {
                        e.dbName = 0;
                        boost::shared_ptr<DurOp> op = DurOp::read(lenOrOpCode, *_entries);
                        if (_doDurOps) {
                            e.op = op;
                        }
                        return;
                    }

                    case JEntry::OpCode_DbContext: {
                        _lastDbName = (const char*) _entries->pos();
                        const unsigned limit = _entries->remaining();
                        const unsigned len = strnlen(_lastDbName, limit);
                        if (_lastDbName[len] != '\0') {
                            log() << "problem processing journal file during recovery";
                            throw JournalSectionCorruptException();
                        }

                        _entries->skip(len+1); // skip '\0' too
                        _entries->read(lenOrOpCode); // read this for the fall through
                    }
                    // fall through as a basic operation always follows jdbcontext, and we don't have anything to return yet

                    default:
                        // fall through
                        ;
                    }
                }

                // JEntry - a basic write
                verify( lenOrOpCode && lenOrOpCode < JEntry::OpCode_Min );
                _entries->rewind(4);
                e.e = (JEntry *) _entries->skip(sizeof(JEntry));
                e.dbName = e.e->isLocalDbContext() ? "local" : _lastDbName;
                verify( e.e->len == lenOrOpCode );
                _entries->skip(e.e->len);
            }


        private:
            auto_ptr<BufReader> _entries;
            const JSectHeader _h;
            const char *_lastDbName; // pointer into mmaped journal file
            const bool _doDurOps;
            string _uncompressed;
        };


        static string fileName(const char* dbName, int fileNo) {
            stringstream ss;
            ss << dbName << '.';
            verify( fileNo >= 0 );
            if( fileNo == JEntry::DotNsSuffix )
                ss << "ns";
            else
                ss << fileNo;

            // relative name -> full path name
            boost::filesystem::path full(storageGlobalParams.dbpath);
            full /= ss.str();
            return full.string();
        }


        RecoveryJob::RecoveryJob()
            : _mx("recovery"),
              _recovering(false),
              _lastDataSyncedFromLastRun(0),
              _lastSeqMentionedInConsoleLog(1) {

        }

        RecoveryJob::~RecoveryJob() {
            DESTRUCTOR_GUARD(
                if (!_mmfs.empty()) {}
                    close();
            )
        }

        void RecoveryJob::close() {
            scoped_lock lk(_mx);
            _close();
        }

        void RecoveryJob::_close() {
            MongoFile::flushAll(true);
            _mmfs.clear();
        }

        RecoveryJob::Last::Last() : mmf(NULL), fileNo(-1) { 
            // Make sure the files list does not change from underneath
            LockMongoFilesShared::assertAtLeastReadLocked();
        }

        DurableMappedFile* RecoveryJob::Last::newEntry(const dur::ParsedJournalEntry& entry, RecoveryJob& rj) {
            int num = entry.e->getFileNo();
            if( num == fileNo && entry.dbName == dbName )
                return mmf;

            string fn = fileName(entry.dbName, num);
            MongoFile *file;
            {
                MongoFileFinder finder; // must release lock before creating new DurableMappedFile
                file = finder.findByPath(fn);
            }

            if (file) {
                verify(file->isDurableMappedFile());
                mmf = (DurableMappedFile*)file;
            }
            else {
                if( !rj._recovering ) {
                    log() << "journal error applying writes, file " << fn << " is not open" << endl;
                    verify(false);
                }
                boost::shared_ptr<DurableMappedFile> sp (new DurableMappedFile);
                verify(sp->open(fn, false));
                rj._mmfs.push_back(sp);
                mmf = sp.get();
            }

            // we do this last so that if an exception were thrown, there isn't any wrong memory
            dbName = entry.dbName;
            fileNo = num;
            return mmf;
        }

        void RecoveryJob::write(Last& last, const ParsedJournalEntry& entry) {
            //TODO(mathias): look into making some of these dasserts
            verify(entry.e);
            verify(entry.dbName);

            DurableMappedFile *mmf = last.newEntry(entry, *this);

            if ((entry.e->ofs + entry.e->len) <= mmf->length()) {
                verify(mmf->view_write());
                verify(entry.e->srcData());

                void* dest = (char*)mmf->view_write() + entry.e->ofs;
                memcpy(dest, entry.e->srcData(), entry.e->len);
                stats.curr()->_writeToDataFilesBytes += entry.e->len;
            }
            else {
                massert(13622, "Trying to write past end of file in WRITETODATAFILES", _recovering);
            }
        }

        void RecoveryJob::applyEntry(Last& last, const ParsedJournalEntry& entry, bool apply, bool dump) {
            if( entry.e ) {
                if( dump ) {
                    stringstream ss;
                    ss << "  BASICWRITE " << setw(20) << entry.dbName << '.';
                    if( entry.e->isNsSuffix() )
                        ss << "ns";
                    else
                        ss << setw(2) << entry.e->getFileNo();
                    ss << ' ' << setw(6) << entry.e->len << ' ' << /*hex << setw(8) << (size_t) fqe.srcData << dec <<*/
                       "  " << hexdump(entry.e->srcData(), entry.e->len);
                    log() << ss.str() << endl;
                }
                if( apply ) {
                    write(last, entry);
              }
            }
            else if(entry.op) {
                // a DurOp subclass operation
                if( dump ) {
                    log() << "  OP " << entry.op->toString() << endl;
                }
                if( apply ) {
                    if( entry.op->needFilesClosed() ) {
                        _close(); // locked in processSection
                    }
                    entry.op->replay();
                }
            }
        }

        void RecoveryJob::applyEntries(const vector<ParsedJournalEntry> &entries) {
            const bool apply =
                (mmapv1GlobalOptions.journalOptions & MMAPV1Options::JournalScanOnly) == 0;
            const bool dump =
                (mmapv1GlobalOptions.journalOptions & MMAPV1Options::JournalDumpJournal);

            if (dump) {
                log() << "BEGIN section" << endl;
            }

            Last last;
            for (vector<ParsedJournalEntry>::const_iterator i = entries.begin(); i != entries.end(); ++i) {
                applyEntry(last, *i, apply, dump);
            }

            if (dump) {
                log() << "END section" << endl;
            }
        }

        void RecoveryJob::processSection(const JSectHeader *h, const void *p, unsigned len, const JSectFooter *f) {
            LockMongoFilesShared lkFiles; // for RecoveryJob::Last
            scoped_lock lk(_mx);

            // Check the footer checksum before doing anything else.
            if (_recovering) {
                verify( ((const char *)h) + sizeof(JSectHeader) == p );
                if (!f->checkHash(h, len + sizeof(JSectHeader))) {
                    log() << "journal section checksum doesn't match";
                    throw JournalSectionCorruptException();
                }
            }

            if( _recovering && _lastDataSyncedFromLastRun > h->seqNumber + ExtraKeepTimeMs ) {
                if( h->seqNumber != _lastSeqMentionedInConsoleLog ) {
                    static int n;
                    if( ++n < 10 ) {
                        log() << "recover skipping application of section seq:" << h->seqNumber << " < lsn:" << _lastDataSyncedFromLastRun << endl;
                    }
                    else if( n == 10 ) { 
                        log() << "recover skipping application of section more..." << endl;
                    }
                    _lastSeqMentionedInConsoleLog = h->seqNumber;
                }
                return;
            }

            auto_ptr<JournalSectionIterator> i;
            if( _recovering ) {
                i = auto_ptr<JournalSectionIterator>(new JournalSectionIterator(*h, p, len, _recovering));
            }
            else { 
                i = auto_ptr<JournalSectionIterator>(new JournalSectionIterator(*h, /*after header*/p, /*w/out header*/len));
            }

            // we use a static so that we don't have to reallocate every time through.  occasionally we 
            // go back to a small allocation so that if there were a spiky growth it won't stick forever.
            static vector<ParsedJournalEntry> entries;
            entries.clear();
/** TEMP uncomment
            RARELY OCCASIONALLY {
                if( entries.capacity() > 2048 ) {
                    entries.shrink_to_fit();
                    entries.reserve(2048);
                }
            }
*/

            // first read all entries to make sure this section is valid
            ParsedJournalEntry e;
            while( !i->atEof() ) {
                i->next(e);
                entries.push_back(e);
            }

            // got all the entries for one group commit.  apply them:
            applyEntries(entries);
        }

        /** apply a specific journal file, that is already mmap'd
            @param p start of the memory mapped file
            @return true if this is detected to be the last file (ends abruptly)
        */
        bool RecoveryJob::processFileBuffer(const void *p, unsigned len) {
            try {
                unsigned long long fileId;
                BufReader br(p,len);

                {
                    // read file header
                    JHeader h;
                    br.read(h);

                    if (!h.valid()) {
                        log() << "Journal file header invalid. This could indicate corruption, or "
                              << "an unclean shutdown while writing the first section in a journal "
                              << "file.";
                        throw JournalSectionCorruptException();
                    }

                    if( !h.versionOk() ) {
                        log() << "journal file version number mismatch got:" << hex << h._version                             
                            << " expected:" << hex << (unsigned) JHeader::CurrentVersion 
                            << ". if you have just upgraded, recover with old version of mongod, terminate cleanly, then upgrade." 
                            << endl;
                        // Not using JournalSectionCurruptException as we don't want to ignore
                        // journal files on upgrade.
                        uasserted(13536, str::stream() << "journal version number mismatch " << h._version);
                    }
                    fileId = h.fileId;
                    if (mmapv1GlobalOptions.journalOptions &
                        MMAPV1Options::JournalDumpJournal) {
                        log() << "JHeader::fileId=" << fileId << endl;
                    }
                }

                // read sections
                while ( !br.atEof() ) {
                    JSectHeader h;
                    br.peek(h);
                    if( h.fileId != fileId ) {
                        if (debug || (mmapv1GlobalOptions.journalOptions &
                                      MMAPV1Options::JournalDumpJournal)) {
                            log() << "Ending processFileBuffer at differing fileId want:" << fileId << " got:" << h.fileId << endl;
                            log() << "  sect len:" << h.sectionLen() << " seqnum:" << h.seqNumber << endl;
                        }
                        return true;
                    }
                    unsigned slen = h.sectionLen();
                    unsigned dataLen = slen - sizeof(JSectHeader) - sizeof(JSectFooter);
                    const char *hdr = (const char *) br.skip(h.sectionLenWithPadding());
                    const char *data = hdr + sizeof(JSectHeader);
                    const char *footer = data + dataLen;
                    processSection((const JSectHeader*) hdr, data, dataLen, (const JSectFooter*) footer);

                    // ctrl c check
                    uassert(ErrorCodes::Interrupted, "interrupted during journal recovery", !inShutdown());
                }
            }
            catch (const BufReader::eof&) {
                if (mmapv1GlobalOptions.journalOptions & MMAPV1Options::JournalDumpJournal)
                    log() << "ABRUPT END" << endl;
                return true; // abrupt end
            }
            catch (const JournalSectionCorruptException&) {
                if (mmapv1GlobalOptions.journalOptions & MMAPV1Options::JournalDumpJournal)
                    log() << "ABRUPT END" << endl;
                return true; // abrupt end
            }

            return false; // non-abrupt end
        }

        /** apply a specific journal file */
        bool RecoveryJob::processFile(boost::filesystem::path journalfile) {
            log() << "recover " << journalfile.string() << endl;

            try { 
                if( boost::filesystem::file_size( journalfile.string() ) == 0 ) {
                    log() << "recover info " << journalfile.string() << " has zero length" << endl;
                    return true;
                }
            } catch(...) { 
                // if something weird like a permissions problem keep going so the massert down below can happen (presumably)
                log() << "recover exception checking filesize" << endl;
            }

            MemoryMappedFile f;
            void *p = f.mapWithOptions(journalfile.string().c_str(), MongoFile::READONLY | MongoFile::SEQUENTIAL);
            massert(13544, str::stream() << "recover error couldn't open " << journalfile.string(), p);
            return processFileBuffer(p, (unsigned) f.length());
        }

        /** @param files all the j._0 style files we need to apply for recovery */
        void RecoveryJob::go(vector<boost::filesystem::path>& files) {
            log() << "recover begin" << endl;
            LockMongoFilesExclusive lkFiles; // for RecoveryJob::Last
            _recovering = true;

            // load the last sequence number synced to the datafiles on disk before the last crash
            _lastDataSyncedFromLastRun = journalReadLSN();
            log() << "recover lsn: " << _lastDataSyncedFromLastRun << endl;

            for( unsigned i = 0; i != files.size(); ++i ) {
                bool abruptEnd = processFile(files[i]);
                if( abruptEnd && i+1 < files.size() ) {
                    log() << "recover error: abrupt end to file " << files[i].string() << ", yet it isn't the last journal file" << endl;
                    close();
                    uasserted(13535, "recover abrupt journal file end");
                }
            }

            close();

            if (mmapv1GlobalOptions.journalOptions & MMAPV1Options::JournalScanOnly) {
                uasserted(13545, str::stream() << "--durOptions "
                                               << (int) MMAPV1Options::JournalScanOnly
                                               << " (scan only) specified");
            }

            log() << "recover cleaning up" << endl;
            removeJournalFiles();
            log() << "recover done" << endl;
            okToCleanUp = true;
            _recovering = false;
        }

        void _recover() {
            verify(storageGlobalParams.dur);

            boost::filesystem::path p = getJournalDir();
            if( !exists(p) ) {
                log() << "directory " << p.string() << " does not exist, there will be no recovery startup step" << endl;
                okToCleanUp = true;
                return;
            }

            vector<boost::filesystem::path> journalFiles;
            getFiles(p, journalFiles);

            if( journalFiles.empty() ) {
                log() << "recover : no journal files present, no recovery needed" << endl;
                okToCleanUp = true;
                return;
            }

            RecoveryJob::get().go(journalFiles);
        }

        /** recover from a crash
            called during startup
            throws on error
        */
        void replayJournalFilesAtStartup() {
            // we use a lock so that exitCleanly will wait for us
            // to finish (or at least to notice what is up and stop)
            OperationContextImpl txn;
            ScopedTransaction transaction(&txn, MODE_X);
            Lock::GlobalWrite lk(txn.lockState());

            _recover(); // throws on interruption
        }

        struct BufReaderY { int a,b; };
        class BufReaderUnitTest : public StartupTest {
        public:
            void run() {
                BufReader r((void*) "abcdabcdabcd", 12);
                char x;
                BufReaderY y;
                r.read(x); //cout << x; // a
                verify( x == 'a' );
                r.read(y);
                r.read(x);
                verify( x == 'b' );
            }
        } brunittest;

    } // namespace dur
} // namespace mongo

