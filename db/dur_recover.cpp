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

#if defined(_DURABLE)

#include "dur.h"
#include "dur_journal.h"
#include "dur_journalformat.h"
#include "durop.h"
#include "namespace.h"
#include "../util/mongoutils/str.h"
#include "bufreader.h"
#include "pdfile.h"
#include "database.h"
#include "db.h"
#include "../util/unittest.h"
#include "cmdline.h"

using namespace mongoutils;

namespace mongo { 

    namespace dur { 

        /** todo clean up : this is messy from refactoring. */
         struct FullyQualifiedJournalEntry { 
             bool isBasicWrite() const { return dbName != 0; }

             const char *dbName;
             JEntry e;
             const char *srcData;

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
        class JournalIterator : boost::noncopyable {
        public:
            JournalIterator(void *p, unsigned len) : _br(p, len) {
                _needHeader = true;
                *_lastDbName = 0;

                JHeader h;
                _br.read(h); // read/skip file header
                uassert(13536, str::stream() << "journal version number mismatch " << h.version, h.versionOk());
                uassert(13537, "journal header invalid", h.valid());
            }

            bool atEof() const { return _br.atEof(); }

            /** get the next entry from the log.  this function parses and combines JDbContext and JEntry's.
             *  @return true if got an entry.  false at successful end of section (and no entry returned).
             *  throws on premature end of section. 
             */
            bool next(FullyQualifiedJournalEntry& e) { 
                if( _needHeader ) {
                    JSectHeader h;
                    _br.read(h);
                    _needHeader = false;
                }

                unsigned lenOrOpCode;
                _br.read(lenOrOpCode);
                if( lenOrOpCode >= JEntry::OpCode_Min ) { 
                    if( lenOrOpCode == JEntry::OpCode_Footer ) { 
                        _needHeader = true;
                        _br.skip(sizeof(JSectFooter) - 4);
                        _br.align(Alignment);
                        return false;
                    }

                    if( lenOrOpCode != JEntry::OpCode_DbContext ) { 
                        e.dbName = 0;
                        e.op = DurOp::read(lenOrOpCode, _br);
                        return true;
                    }

                    // JDbContext
                    {
                        char c;
                        char *p = _lastDbName;
                        char *end = p + Namespace::MaxNsLen;
                        while( 1 ) { 
                            _br.read(c);
                            *p++ = c;
                            if( c == 0 )
                                break;
                            if( p >= end ) { 
                                /* this shouldn't happen, even if log is truncated. */
                                log() << _br.offset() << endl;
                                uasserted(13533, "problem processing journal file during recovery");
                            }
                        }

                        _br.read(lenOrOpCode);
                    }
                }

                // now do JEntry
                assert( lenOrOpCode && lenOrOpCode < JEntry::OpCode_Min );
                e.dbName = _lastDbName;
                e.e.len = lenOrOpCode;
                _br.read(e.e.ofs);
                _br.read(e.e.fileNo);
                e.srcData = (const char *) _br.skip(lenOrOpCode);
                return true;
            }
        private:
            bool _needHeader;
            BufReader _br;
            char _lastDbName[Namespace::MaxNsLen];
        };

       /** call go() to execute a recovery from existing journal files.
        */
        class RecoveryJob : boost::noncopyable { 
        public:
            void go(vector<path>& files);
            ~RecoveryJob();
        private:
            void applyEntries(const vector<FullyQualifiedJournalEntry> &entries);
            bool processBuffer(void *, unsigned len);
            bool processFile(path journalfile);
            void close();

            /** retrieve the mmap pointer for the specified dbName plus file number.
                open if not yet open.
                @param fileNo a value of -1 indicates ".ns"
                @param ofs offset to add to the pointer before returning
            */
            void* ptr(const char *dbName, int fileNo, unsigned ofs);

            map< pair<int,string>, void* > _fileToPtr;
            list< shared_ptr<MemoryMappedFile> > _files;
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
                    if( fileNo < 0 )
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
                p = f->map(fn.c_str());
                uassert(13534, str::stream() << "recovery error couldn't open " << fn, p);
                if( cmdLine.durTrace & CmdLine::DurDumpJournal ) 
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

        string hexdump(const char *data, unsigned len) {
            const unsigned char *p = (const unsigned char *) data;
            stringstream ss;
            for( unsigned i = 0; i < 4 && i < len; i++ ) {
                ss << std::hex << setw(2) << setfill('0');
                unsigned n = p[i];
                ss << n;
                ss << ' ';
            }
            string s = ss.str();
            return s;
        }

        void RecoveryJob::applyEntries(const vector<FullyQualifiedJournalEntry> &entries) { 
            bool apply = (cmdLine.durTrace & CmdLine::DurScanOnly) == 0;
            bool dump = cmdLine.durTrace & CmdLine::DurDumpJournal;
            if( dump )
                log() << "BEGIN section" << endl;
            
            for( vector<FullyQualifiedJournalEntry>::const_iterator i = entries.begin(); i != entries.end(); ++i ) { 
                const FullyQualifiedJournalEntry& fqe = *i;
                if( fqe.isBasicWrite() ) {
                    if( dump ) {
                        stringstream ss;
                        ss << "  BASICWRITE " << setw(20) << fqe.dbName << '.' 
                              << setw(2) << fqe.e.fileNo << ' ' << setw(6) << fqe.e.len
                              << ' ' << hex << setw(8) << (size_t) fqe.srcData << dec << "  " << hexdump(fqe.srcData, fqe.e.len);
                        log() << ss.str() << endl;
                    } 
                    if( apply ) {
                        void *p = ptr(fqe.dbName, fqe.e.fileNo, fqe.e.ofs);
                        memcpy(p, fqe.srcData, fqe.e.len);
                    }
                } else {
                    if( dump ) {
                        log() << "  OP " << fqe.op->toString() << endl;
                    } 
                    if( apply ) {
                        fqe.op->replay();
                    }
                }
            }            

            if( dump )
                log() << "END section" << endl;
        }

        /** @param p start of the memory mapped file
            @return true if this is detected to be the last file (ends abruptly) 
        */
        bool RecoveryJob::processBuffer(void *p, unsigned len) {
            JournalIterator i(p, len);
            vector<FullyQualifiedJournalEntry> entries;

            try {
                while( 1 ) { 
                    entries.clear();

                    FullyQualifiedJournalEntry e;
                    while( i.next(e) )
                        entries.push_back(e);

                    // got all the entries for one group commit.  apply them: 
                    applyEntries(entries);

                    // now do the next section (i.e. group commit)
                    if( i.atEof() )
                        break;
                }
            }
            catch( BufReader::eof& ) { 
                if( cmdLine.durTrace & CmdLine::DurDumpJournal )
                    log() << "ABRUPT END" << endl;
                return true; // abrupt end
            }

            return false; // non-abrupt end
        }

        bool RecoveryJob::processFile(path journalfile) {
            log() << "recover " << journalfile.string() << endl;
            MemoryMappedFile f;
            void *p = f.mapWithOptions(journalfile.string().c_str(), MongoFile::READONLY | MongoFile::SEQUENTIAL);
            massert(13544, str::stream() << "recover error couldn't open " << journalfile.string(), p);
            return processBuffer(p, (unsigned) f.length());
        }

        void RecoveryJob::go(vector<path>& files) { 
            log() << "recover begin" << endl;

            for( unsigned i = 0; i != files.size(); ++i ) { 
                bool abruptEnd = processFile(files[i]);
                if( abruptEnd && i+1 < files.size() ) { 
                    log() << "recover error: abrupt end to file " << files[i].string() << ", yet it isn't the last journal file" << endl;
                    close();
                    uasserted(13535, "recover abrupt journal file end");
                }
            }

            close();

            if( cmdLine.durTrace & CmdLine::DurScanOnly ) {
                uasserted(13545, str::stream() << "--durTrace " << (int) CmdLine::DurScanOnly << " specified, terminating");
            }

            log() << "recover cleaning up" << endl;
            removeJournalFiles();
            log() << "recover done" << endl;
            okToCleanUp = true;
        }

        /** recover from a crash
            throws on error 
        */
        void recover() {
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

            RecoveryJob j;
            j.go(journalFiles);
       }

        struct BufReaderY { int a,b; };
        class BufReaderUnitTest : public UnitTest {
        public:
            void run() { 
                BufReader r((void*) "abcdabcdabcd", 12);
                char x;
                BufReaderY y;
                r.read(x); cout << x; // a
                assert( x == 'a' );
                r.read(y);
                r.read(x); 
                assert( x == 'b' );
            }
        } brunittest;

    } // namespace dur

} // namespace mongo

#endif
