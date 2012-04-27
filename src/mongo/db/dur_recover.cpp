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
*/

#include "pch.h"

#include "dur.h"
#include "dur_stats.h"
#include "dur_recover.h"
#include "dur_journal.h"
#include "dur_journalformat.h"
#include "durop.h"
#include "namespace.h"
#include "../util/mongoutils/str.h"
#include "../util/bufreader.h"
#include "../util/concurrency/race.h"
#include "pdfile.h"
#include "database.h"
#include "db.h"
#include "../util/startup_test.h"
#include "../util/checksum.h"
#include "cmdline.h"
#include "curop.h"
#include "mongommf.h"
#include "../util/compress.h"
#include <sys/stat.h>
#include <fcntl.h>
#include "dur_commitjob.h"
#include <boost/filesystem/operations.hpp>

using namespace mongoutils;

namespace mongo {

    namespace dur {

        struct ParsedJournalEntry { /*copyable*/
            ParsedJournalEntry() : e(0) { }

            // relative path of database for the operation.
            // might be a pointer into mmaped Journal file
            const char *dbName;

            // thse are pointers into the memory mapped journal file
            const JEntry *e;  // local db sentinel is already parsed out here into dbName

            // if not one of the two simple JEntry's above, this is the operation:
            shared_ptr<DurOp> op;
        };

        void removeJournalFiles();
        boost::filesystem::path getJournalDir();

        /** get journal filenames, in order. throws if unexpected content found */
        static void getFiles(boost::filesystem::path dir, vector<boost::filesystem::path>& files) {
            map<unsigned,boost::filesystem::path> m;
            for ( boost::filesystem::directory_iterator i( dir );
                    i != boost::filesystem::directory_iterator();
                    ++i ) {
                boost::filesystem::path filepath = *i;
                string fileName = boost::filesystem::path(*i).leaf();
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
                      << " : " << boost::filesystem::path(i->second).leaf() << " : can't find its preceeding file");
                }
                files.push_back(i->second);
            }
        }

        /** read through the memory mapped data of a journal file (journal/j._<n> file)
            throws
        */
        class JournalSectionIterator : boost::noncopyable {
            auto_ptr<BufReader> _entries;
            const JSectHeader _h;
            const char *_lastDbName; // pointer into mmaped journal file
            const bool _doDurOps;
            string _uncompressed;
        public:
            JournalSectionIterator(const JSectHeader& h, const void *compressed, unsigned compressedLen, bool doDurOpsRecovering) :
                _h(h),
                _lastDbName(0)
                , _doDurOps(doDurOpsRecovering)
            {
                verify( doDurOpsRecovering );
                bool ok = uncompress((const char *)compressed, compressedLen, &_uncompressed);
                if( !ok ) { 
                    // it should always be ok (i think?) as there is a previous check to see that the JSectFooter is ok
                    log() << "couldn't uncompress journal section" << endl;
                    msgasserted(15874, "couldn't uncompress journal section");
                }
                const char *p = _uncompressed.c_str();
                verify( compressedLen == _h.sectionLen() - sizeof(JSectFooter) - sizeof(JSectHeader) );
                _entries = auto_ptr<BufReader>( new BufReader(p, _uncompressed.size()) );
            }

            // we work with the uncompressed buffer when doing a WRITETODATAFILES (for speed)
            JournalSectionIterator(const JSectHeader &h, const void *p, unsigned len) :
                _entries( new BufReader((const char *) p, len) ),
                _h(h),
                _lastDbName(0)
                , _doDurOps(false)

                { }

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
                        const unsigned limit = std::min((unsigned)Namespace::MaxNsLen, _entries->remaining());
                        const unsigned len = strnlen(_lastDbName, limit);
                        massert(13533, "problem processing journal file during recovery", _lastDbName[len] == '\0');
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
            boost::filesystem::path full(dbpath);
            full /= ss.str();
            return full.string();
        }

        RecoveryJob::~RecoveryJob() {
            DESTRUCTOR_GUARD(
                if( !_mmfs.empty() )
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

        void RecoveryJob::write(const ParsedJournalEntry& entry) {
            //TODO(mathias): look into making some of these dasserts
            verify(entry.e);
            verify(entry.dbName);
            verify(strnlen(entry.dbName, MaxDatabaseNameLen) < MaxDatabaseNameLen);

            const string fn = fileName(entry.dbName, entry.e->getFileNo());
            MongoFile* file;
            {
                MongoFileFinder finder; // must release lock before creating new MongoMMF
                file = finder.findByPath(fn);
            }

            MongoMMF* mmf;
            if (file) {
                verify(file->isMongoMMF());
                mmf = (MongoMMF*)file;
            }
            else {
                if( !_recovering ) {
                    log() << "journal error applying writes, file " << fn << " is not open" << endl;
                    verify(false);
                }
                boost::shared_ptr<MongoMMF> sp (new MongoMMF);
                verify(sp->open(fn, false));
                _mmfs.push_back(sp);
                mmf = sp.get();
            }

            if ((entry.e->ofs + entry.e->len) <= mmf->length()) {
                verify(mmf->view_write());
                verify(entry.e->srcData());

                void* dest = (char*)mmf->view_write() + entry.e->ofs;
                memcpy(dest, entry.e->srcData(), entry.e->len);
                stats.curr->_writeToDataFilesBytes += entry.e->len;
            }
            else {
                massert(13622, "Trying to write past end of file in WRITETODATAFILES", _recovering);
            }
        }

        void RecoveryJob::applyEntry(const ParsedJournalEntry& entry, bool apply, bool dump) {
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
                    write(entry);
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
            bool apply = (cmdLine.durOptions & CmdLine::DurScanOnly) == 0;
            bool dump = cmdLine.durOptions & CmdLine::DurDumpJournal;
            if( dump )
                log() << "BEGIN section" << endl;

            for( vector<ParsedJournalEntry>::const_iterator i = entries.begin(); i != entries.end(); ++i ) {
                applyEntry(*i, apply, dump);
            }

            if( dump )
                log() << "END section" << endl;
        }

        void RecoveryJob::processSection(const JSectHeader *h, const void *p, unsigned len, const JSectFooter *f) {
            scoped_lock lk(_mx);
            RACECHECK

            /** todo: we should really verify the checksum to see that seqNumber is ok?
                      that is expensive maybe there is some sort of checksum of just the header 
                      within the header itself
            */
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

            // after the entries check the footer checksum
            if( _recovering ) {
                verify( ((const char *)h) + sizeof(JSectHeader) == p );
                if( !f->checkHash(h, len + sizeof(JSectHeader)) ) { 
                    msgasserted(13594, "journal checksum doesn't match");
                }
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

                    /* [dm] not automatically handled.  we should eventually handle this automatically.  i think:
                       (1) if this is the final journal file
                       (2) and the file size is just the file header in length (or less) -- this is a bit tricky to determine if prealloced
                       then can just assume recovery ended cleanly and not error out (still should log).
                    */
                    uassert(13537, 
                        "journal file header invalid. This could indicate corruption in a journal file, or perhaps a crash where sectors in file header were in flight written out of order at time of crash (unlikely but possible).", 
                        h.valid());

                    if( !h.versionOk() ) {
                        log() << "journal file version number mismatch got:" << hex << h._version                             
                            << " expected:" << hex << (unsigned) JHeader::CurrentVersion 
                            << ". if you have just upgraded, recover with old version of mongod, terminate cleanly, then upgrade." 
                            << endl;
                        uasserted(13536, str::stream() << "journal version number mismatch " << h._version);
                    }
                    fileId = h.fileId;
                    if(cmdLine.durOptions & CmdLine::DurDumpJournal) { 
                        log() << "JHeader::fileId=" << fileId << endl;
                    }
                }

                // read sections
                while ( !br.atEof() ) {
                    JSectHeader h;
                    br.peek(h);
                    if( h.fileId != fileId ) {
                        if( debug || (cmdLine.durOptions & CmdLine::DurDumpJournal) ) {
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
                    killCurrentOp.checkForInterrupt(false);
                }
            }
            catch( BufReader::eof& ) {
                if( cmdLine.durOptions & CmdLine::DurDumpJournal )
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

            if( cmdLine.durOptions & CmdLine::DurScanOnly ) {
                uasserted(13545, str::stream() << "--durOptions " << (int) CmdLine::DurScanOnly << " (scan only) specified");
            }

            log() << "recover cleaning up" << endl;
            removeJournalFiles();
            log() << "recover done" << endl;
            okToCleanUp = true;
            _recovering = false;
        }

        void _recover() {
            verify( cmdLine.dur );

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
        void recover() {
            // we use a lock so that exitCleanly will wait for us
            // to finish (or at least to notice what is up and stop)
            Lock::GlobalWrite lk;

            // this is so the mutexdebugger doesn't get confused.  we are actually single threaded 
            // at this point in the program so it wouldn't have been a true problem (I think)
            
            // can't lock groupCommitMutex here as 
            //   MongoMMF::close()->closingFileNotication()->groupCommit() will lock it
            //   and that would be recursive.
            //   
            // SimpleMutex::scoped_lock lk2(commitJob.groupCommitMutex);

            _recover(); // throws on interruption
        }

        struct BufReaderY { little<int> a,b; };
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

        // can't free at termination because order of destruction of global vars is arbitrary
        RecoveryJob &RecoveryJob::_instance = *(new RecoveryJob());

    } // namespace dur

} // namespace mongo

