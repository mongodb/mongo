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
#include "../util/mongoutils/str.h"

using namespace mongoutils;

namespace mongo { 

    namespace dur { 

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
                        uasserted(10000, str::stream() << "unexpected files in journal directory " << dir.string() << " : " << fileName);
                    }
                    if( !m.empty() && !m.count(u-1) ) { 
                        uasserted(10000, 
                            str::stream() << "unexpected file in journal directory " << dir.string() 
                            << " : " << fileName << " : can't find its preceeding file");
                    }
                    m.insert( pair<unsigned,path>(u,filepath) );
                }
            }
            for( map<unsigned,path>::iterator i = m.begin(); i != m.end(); ++i ) 
                files.push_back(i->second);
        }

        /** call go() to execute a recovery 
        */
        class RecoveryJob : boost::noncopyable { 
        public:
            void go(vector<path>& files);
        private:
            bool _apply(void *, void *end);
            void apply(path journalfile);
            void close();
        };

        /** @param p start of the memory mapped file
            @param end end of the memory mapped file
            @return true if this is detected to be the last file (ends abruptly) 
        */
        bool RecoveryJob::_apply(void *p, void *end) {
            JHeader *h = (JHeader *) p;
            if( end < h+1 )
                return true;
            log() << h->ts << endl;
            log() << h->dbpath << endl;

            unsigned n = 0;
            JSectHeader *sect = (JSectHeader *) (h+1);
            while( 1 ) {
                JEntry *e = (JEntry*) (sect+1);
                if( end < e ) 
                    return true;
                unsigned len = sect->len;

                char *p = ((char *)sect) + len;
                if( end < p ) 
                    return true;

                JSectFooter *f = (JSectFooter *) (p-sizeof(JSectFooter));
                assert( f > (void*) e );

                // ...
            }

            return false;
        }

        void RecoveryJob::apply(path journalfile) {
            MemoryMappedFile f;
            void *p = f.mapWithOptions(journalfile.string().c_str(), MongoFile::READONLY | MongoFile::SEQUENTIAL);
            assert(p);
            _apply(p, ((char *)p)+f.length());
        }

        void RecoveryJob::close() { 
            MongoFile::flushAll(true);
            // ...
        }
        
        void RecoveryJob::go(vector<path>& files) { 
            log() << "recovery begin" << endl;

            for( vector<path>::iterator i = files.begin(); i != files.end(); ++i ) { 
                apply(*i);
            }

            close();
            log() << "recovery cleaning up" << endl;
            removeJournalFiles();
            log() << "recovery done" << endl;
        }

        /** recover from a crash
            throws on error 
        */
        void recover() { 
            assert( durable );

            filesystem::path p = getJournalDir();
            if( !exists(p) ) { 
                log() << "directory " << p.string() << " does not exist, there will be no recovery startup step" << endl;
                return;
            }

            vector<path> files;
            getFiles(p, files);

            if( files.empty() ) { 
                log() << "recovery : no journal files present, no recovery needed" << endl;
                return;
            }

            RecoveryJob j;
            j.go(files);
       }

    } // namespace dur

} // namespace mongo

#endif
