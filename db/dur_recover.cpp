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
#include "dur_recover.h"
#include "dur_journal.h"
#include "dur_journalformat.h"
#include "durop.h"
#include "namespace.h"
#include "../util/mongoutils/str.h"
#include "../util/bufreader.h"
#include "pdfile.h"
#include "database.h"
#include "db.h"
#include "../util/unittest.h"
#include "cmdline.h"
#include "curop.h"

using namespace mongoutils;

namespace mongo { 

    namespace dur { 

        struct ParsedJournalEntry /*copyable*/ { 
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
        path getJournalDir();

        /** get journal filenames, in order. throws if unexpected content found */
        static void getFiles(path dir, vector<path>& files) { 
            map<unsigned,path> m;
            for ( filesystem::directory_iterator i( dir );
                  i != filesystem::directory_iterator(); 
                  ++i ) {
                filesystem::path filepath = *i;
                string fileName = filesystem::path(*i).leaf();
                if( str::startsWith(fileName, "j._") ) {
                    unsigned u = str::toUnsigned( str::after(fileName, '_') );
                    if( m.count(u) ) { 
                        uasserted(13531, str::stream() << "unexpected files in journal directory " << dir.string() << " : " << fileName);
                    }
                    if( !m.empty() && !m.count(u-1) ) { 
                        uasserted(13532, 
                            str::stream() << "unexpected file in journal directory " << dir.string() 
                            << " : " << fileName << " : can't find its preceeding file");
                    }
                    m.insert( pair<unsigned,path>(u,filepath) );
                }
            }
            for( map<unsigned,path>::iterator i = m.begin(); i != m.end(); ++i ) 
                files.push_back(i->second);
        }

        /** read through the memory mapped data of a journal file (journal/j._<n> file)
            throws 
        */
        class JournalSectionIterator : boost::noncopyable {
        public:
            JournalSectionIterator(const void *p, unsigned len)
                : _br(p, len)
                , _sectHead(static_cast<const JSectHeader*>(_br.skip(sizeof(JSectHeader))))
                , _lastDbName(NULL)
            {}

            bool atEof() const { return _br.atEof(); }

            unsigned long long seqNumber() const { return _sectHead->seqNumber; }

            /** get the next entry from the log.  this function parses and combines JDbContext and JEntry's.
             *  @return true if got an entry.  false at successful end of section (and no entry returned).
             *  throws on premature end of section. 
             */
            bool next(ParsedJournalEntry& e) { 
                unsigned lenOrOpCode;
                _br.read(lenOrOpCode);

                switch( lenOrOpCode ) {

                case JEntry::OpCode_Footer:
                    { 
                        const char* pos = (const char*) _br.pos();
                        pos -= sizeof(lenOrOpCode); // rewind to include OpCode
                        const JSectFooter& footer = *(const JSectFooter*)pos;
                        int len = pos - (char*)_sectHead;
                        if (!footer.checkHash(_sectHead, len)){
                            massert(13594, str::stream() << "Journal checksum doesn't match. recorded: "
                                << toHex(footer.hash, sizeof(footer.hash))
                                << " actual: " << md5simpledigest(_sectHead, len)
                                , false);
                        }
                        return false; // false return value denotes end of section
                    }

                case JEntry::OpCode_FileCreated:
                case JEntry::OpCode_DropDb:
                    { 
                        e.dbName = 0;
                        e.op = DurOp::read(lenOrOpCode, _br);
                        return true;
                    }

                case JEntry::OpCode_DbContext:
                    {
                        _lastDbName = (const char*) _br.pos();
                        const unsigned limit = std::min((unsigned)Namespace::MaxNsLen, _br.remaining());
                        const unsigned len = strnlen(_lastDbName, limit);
                        massert(13533, "problem processing journal file during recovery", _lastDbName[len] == '\0');
                        _br.skip(len+1); // skip '\0' too
                        _br.read(lenOrOpCode);
                    }
                    // fall through as a basic operation always follows jdbcontext, and we don't have anything to return yet

                default:
                    // fall through
                    ;
                }

                // JEntry - a basic write
                assert( lenOrOpCode && lenOrOpCode < JEntry::OpCode_Min );
                _br.rewind(4);
                e.e = (JEntry *) _br.skip(sizeof(JEntry));
                e.dbName = e.e->isLocalDbContext() ? "local" : _lastDbName;
                assert( e.e->len == lenOrOpCode );
                _br.skip(e.e->len);
                return true;
            }
        private:
            BufReader _br;
            const JSectHeader* _sectHead;
            const char *_lastDbName; // pointer into mmaped journal file
        };

        
        /** retrieve the mmap pointer for the specified dbName plus file number.
            open if not yet open.
        */
        void* RecoveryJob::ptr(const char *dbName, int fileNo, unsigned ofs) {
            void *&p = _fileToPtr[ pair<int,string>(fileNo,dbName) ];

            if( p == 0 ) {
                MemoryMappedFile *f = new MemoryMappedFile();
                _files.push_back( shared_ptr<MemoryMappedFile>(f) );
                string fn;
                {
                    stringstream ss;
                    ss << dbName << '.';
                    assert( fileNo >= 0 );
                    if( fileNo == JEntry::DotNsSuffix )
                        ss << "ns";
                    else 
                        ss << fileNo;
                    /* todo: do we need to create file here if DNE?
                             we need to know what its length should be for that though. 
                             however does this happen?  FileCreatedOp should have been in the journal and 
                             already applied if the file is new.
                    */
                    fn = ss.str();
                }
                path full(dbpath);
                try {
                    // relative name -> full path name
                    full /= fn;
                    p = f->map(full.string().c_str());
                }
                catch(DBException&) { 
                    log() << "recover error opening file " << full.string() << endl;
                    throw;
                }
                uassert(13534, str::stream() << "recovery error couldn't open " << fn, p);
                if( cmdLine.durOptions & CmdLine::DurDumpJournal ) 
                    log() << "  opened " << fn << ' ' << f->length()/1024.0/1024.0 << endl;
                uassert(13543, str::stream() << "recovery error file has length zero " << fn, f->length());
                assert( ofs < f->length() );
            }

            return ((char*) p) + ofs;
        }

        RecoveryJob::~RecoveryJob() { 
            if( !_files.empty() )
                close();
        }

        void RecoveryJob::close() { 
            log() << "recover flush" << endl;
            MongoFile::flushAll(true);
            log() << "recover close" << endl;
            _files.clear(); // closes files
            _fileToPtr.clear();
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
                    void *p = ptr(entry.dbName, entry.e->getFileNo(), entry.e->ofs);
                    memcpy(p, entry.e->srcData(), entry.e->len);
                }
            } 
            else {
                // a DurOp subclass operation
                if( dump ) {
                    log() << "  OP " << entry.op->toString() << endl;
                } 
                if( apply ) {
                    if( entry.op->needFilesClosed() ) {
                        close();
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

        void RecoveryJob::processSection(const void *p, unsigned len) {
            vector<ParsedJournalEntry> entries;
            JournalSectionIterator i(p, len);

            if( _lastDataSyncedFromLastRun > i.seqNumber() + ExtraKeepTimeMs ) { 
                log() << "recover skipping application of section " << i.seqNumber() << " < lsn:" << _lastDataSyncedFromLastRun << endl;
            }

            // first read all entries to make sure this section is valid
            ParsedJournalEntry e;
            while( i.next(e) ) {
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
                BufReader br(p,len);

                { // read file header
                    JHeader h;
                    br.read(h);
                    if( !h.versionOk() ) {
                        log() << "journal file version number mismatch. recover with old version of mongod, terminate cleanly, then upgrade." << endl;
                        uasserted(13536, str::stream() << "journal version number mismatch " << h._version);
                    }
                    uassert(13537, "journal header invalid", h.valid());
                }

                // read sections
                while ( !br.atEof() ) {
                    JSectHeader h;
                    br.peek(h);
                    processSection(br.skip(h.len), h.len);

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
        bool RecoveryJob::processFile(path journalfile) {
            log() << "recover " << journalfile.string() << endl;
            MemoryMappedFile f;
            void *p = f.mapWithOptions(journalfile.string().c_str(), MongoFile::READONLY | MongoFile::SEQUENTIAL);
            massert(13544, str::stream() << "recover error couldn't open " << journalfile.string(), p);
            return processFileBuffer(p, (unsigned) f.length());
        }

        /** @param files all the j._0 style files we need to apply for recovery */
        void RecoveryJob::go(vector<path>& files) { 
            log() << "recover begin" << endl;

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
        }

        void _recover() {
            assert( cmdLine.dur );

            filesystem::path p = getJournalDir();
            if( !exists(p) ) { 
                log() << "directory " << p.string() << " does not exist, there will be no recovery startup step" << endl;
                okToCleanUp = true;
                return;
            }

            vector<path> journalFiles;
            getFiles(p, journalFiles);

            if( journalFiles.empty() ) { 
                log() << "recover : no journal files present, no recovery needed" << endl;
                okToCleanUp = true;
                return;
            }

            RecoveryJob::get().go(journalFiles);
        }

        /** recover from a crash
            throws on error 
        */
        void recover() { 
            // we use a lock so that exitCleanly will wait for us
            // to finish (or at least to notice what is up and stop)
            readlock lk;
            _recover(); // throws on interruption
        }

        struct BufReaderY { int a,b; };
        class BufReaderUnitTest : public UnitTest {
        public:
            void run() { 
                BufReader r((void*) "abcdabcdabcd", 12);
                char x;
                BufReaderY y;
                r.read(x); //cout << x; // a
                assert( x == 'a' );
                r.read(y);
                r.read(x); 
                assert( x == 'b' );
            }
        } brunittest;

        
        RecoveryJob RecoveryJob::_instance;

    } // namespace dur

} // namespace mongo

