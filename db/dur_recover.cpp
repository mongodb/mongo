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
#include "namespace.h"
#include "../util/mongoutils/str.h"
#include "bufreader.h"
#include "pdfile.h"
#include "database.h"
#include "db.h"
#include "../util/unittest.h"

using namespace mongoutils;

namespace mongo { 

    namespace dur { 

        class BufReaderUnitTest : public UnitTest {
        public:
            void run() { 
                BufReader r("abcdabcdabcd", 12);
                char x;
                struct Y { int a,b; } y;
                r.read(x); cout << x; // a
                assert( x == 'a' );
                r.read(y);
                r.read(x); 
                assert( x == 'b' );
            }
        } brunittest;

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

         struct FullyQualifiedJournalEntry { 
            const char *dbName;
            unsigned len;
            int fileNo;
            const char *data;
        };

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

                unsigned len;
                _br.read(len);
                if( len >= JEntry::Sentinel_Min ) { 
                    if( len == JEntry::Sentinel_Footer ) { 
                        _needHeader = true;
                        _br.skip(sizeof(JSectFooter) - 4);
                        return false;
                    }

                    // JDbContext
                    assert( len == JEntry::Sentinel_Context );
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

                    _br.read(len);
                }

                // now do JEntry
                assert( len && len < JEntry::Sentinel_Min );
                e.dbName = _lastDbName;
                e.len = len;
                _br.read(e.fileNo);
                e.data = (const char *) _br.skip(len);
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
            void __apply(const vector<FullyQualifiedJournalEntry> &entries);
            bool _apply(void *, unsigned len);
            bool apply(path journalfile);
            void close();

            /** retrieve the mmap pointer for the specified dbName plus file number 
                open if not yet open
            */
            void* ptr(const char *dbName, int fileNo);

            map< pair<int,string>, void* > _fileToPtr;
            list< MemoryMappedFile* > _files;
        };
        
        /** retrieve the mmap pointer for the specified dbName plus file number 
            open if not yet open
        */
        void* RecoveryJob::ptr(const char *dbName, int fileNo) {
            void *&p = _fileToPtr[ pair<int,string>(fileNo,dbName) ];
            if( p )
                return p;

            MemoryMappedFile *f = new MemoryMappedFile();
            _files.push_back(f);
            stringstream ss;
            ss << dbName << '.';
            if( fileNo < 0 )
                ss << "ns";
            else 
                ss << fileNo;
            /* todo: need to create file here if DNE. need to know what its length should be for that though. */
            p = f->map(ss.str().c_str());
            uassert(13534, str::stream() << "recovery error couldn't open " << ss.str(), p);
            return p;
        }

        RecoveryJob::~RecoveryJob() { 
            if( !_files.empty() )
                close();
        }

        void RecoveryJob::close() { 
            log() << "recover flush" << endl;
            MongoFile::flushAll(true);
            log() << "recover close" << endl;
            for( list<MemoryMappedFile*>::iterator i = _files.begin(); i != _files.end(); ++i ) {
                delete *i;
            }
            _files.clear();
            _fileToPtr.clear();
        }
        
        void RecoveryJob::__apply(const vector<FullyQualifiedJournalEntry> &entries) { 
            for( vector<FullyQualifiedJournalEntry>::const_iterator i = entries.begin(); i != entries.end(); ++i ) { 
                const FullyQualifiedJournalEntry& e = *i;
                memcpy(ptr(e.dbName, e.fileNo), e.data, e.len);
            }            
        }

        /** @param p start of the memory mapped file
            @return true if this is detected to be the last file (ends abruptly) 
        */
        bool RecoveryJob::_apply(void *p, unsigned len) {
            JournalIterator i(p, len);
            vector<FullyQualifiedJournalEntry> entries;

            try {
                while( 1 ) { 
                    entries.clear();
                    FullyQualifiedJournalEntry e;
                    while( i.next(e) )
                        entries.push_back(e);
                    __apply(entries);
                    if( i.atEof() )
                        break;
                }
            }
            catch( BufReader::eof& ) { 
                return true; // abrupt end
            }

            return false; // non-abrupt end
        }

        bool RecoveryJob::apply(path journalfile) {
            log() << "recover " << journalfile.string() << endl;
            MemoryMappedFile f;
            void *p = f.mapWithOptions(journalfile.string().c_str(), MongoFile::READONLY | MongoFile::SEQUENTIAL);
            assert(p);
            return _apply(p, (unsigned) f.length());
        }

        void RecoveryJob::go(vector<path>& files) { 
            log() << "recover begin" << endl;

            for( unsigned i = 0; i != files.size(); ++i ) { 
                bool abruptEnd = apply(files[i]);
                if( abruptEnd && i+1 < files.size() ) { 
                    log() << "recover error: abrupt end to file " << files[i].string() << ", yet it isn't the last journal file" << endl;
                    close();
                    uasserted(13535, "recover abrupt journal file end");
                }
            }

            close();
            log() << "recover cleaning up" << endl;
            removeJournalFiles();
            log() << "recover done" << endl;
        }

        /** recover from a crash
            throws on error 
        */
        void /*dur::*/recover() { 
            assert( durable );

            filesystem::path p = getJournalDir();
            if( !exists(p) ) { 
                log() << "directory " << p.string() << " does not exist, there will be no recovery startup step" << endl;
                return;
            }

            vector<path> journalFiles;
            getFiles(p, journalFiles);

            if( journalFiles.empty() ) { 
                log() << "recover : no journal files present, no recovery needed" << endl;
                return;
            }

            RecoveryJob j;
            j.go(journalFiles);
       }

    } // namespace dur

} // namespace mongo

#endif
