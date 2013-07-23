// mmap.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/pch.h"

#include "mongo/util/mmap.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/db/cmdline.h"
#include "mongo/util/concurrency/rwlock.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/startup_test.h"

namespace mongo {

    void minOSPageSizeBytesTest(size_t minOSPageSizeBytes) {
        fassert( 16325, minOSPageSizeBytes > 0 );
        fassert( 16326, minOSPageSizeBytes < 1000000 );
        // check to see if the page size is a power of 2
        fassert( 16327, (minOSPageSizeBytes & (minOSPageSizeBytes - 1)) == 0);
    }

namespace {
    set<MongoFile*> mmfiles;
    map<string,MongoFile*> pathToFile;
}  // namespace

    /* Create. Must not exist.
    @param zero fill file with zeros when true
    */
    void* MemoryMappedFile::create(const std::string& filename, unsigned long long len, bool zero) {
        uassert( 13468, string("can't create file already exists ") + filename, ! boost::filesystem::exists(filename) );
        void *p = map(filename.c_str(), len);
        if( p && zero ) {
            size_t sz = (size_t) len;
            verify( len == sz );
            memset(p, 0, sz);
        }
        return p;
    }

    /*static*/ void MemoryMappedFile::updateLength( const char *filename, unsigned long long &length ) {
        if ( !boost::filesystem::exists( filename ) )
            return;
        // make sure we map full length if preexisting file.
        boost::uintmax_t l = boost::filesystem::file_size( filename );
        length = l;
    }

    void* MemoryMappedFile::map(const char *filename) {
        unsigned long long l;
        try {
            l = boost::filesystem::file_size( filename );
        } 
        catch(boost::filesystem::filesystem_error& e) { 
            uasserted(15922, mongoutils::str::stream() << "couldn't get file length when opening mapping " << filename << ' ' << e.what() );
        }
        return map( filename , l );
    }
    void* MemoryMappedFile::mapWithOptions(const char *filename, int options) {
        unsigned long long l;
        try {
            l = boost::filesystem::file_size( filename );
        } 
        catch(boost::filesystem::filesystem_error& e) { 
            uasserted(15923, mongoutils::str::stream() << "couldn't get file length when opening mapping " << filename << ' ' << e.what() );
        }
        return map( filename , l, options );
    }

    /* --- MongoFile -------------------------------------------------
       this is the administrative stuff
    */

    RWLockRecursiveNongreedy LockMongoFilesShared::mmmutex("mmmutex",10*60*1000 /* 10 minutes */);
    unsigned LockMongoFilesShared::era = 99; // note this rolls over

    set<MongoFile*>& MongoFile::getAllFiles() { return mmfiles; }

    /* subclass must call in destructor (or at close).
        removes this from pathToFile and other maps
        safe to call more than once, albeit might be wasted work
        ideal to call close to the close, if the close is well before object destruction
    */
    void MongoFile::destroyed() {
        LockMongoFilesShared::assertExclusivelyLocked();
        mmfiles.erase(this);
        pathToFile.erase( filename() );
    }

    /*static*/
    void MongoFile::closeAllFiles( stringstream &message ) {
        static int closingAllFiles = 0;
        if ( closingAllFiles ) {
            message << "warning closingAllFiles=" << closingAllFiles << endl;
            return;
        }
        ++closingAllFiles;

        LockMongoFilesExclusive lk;

        ProgressMeter pm(mmfiles.size(), 2, 1, "files", "File Closing Progress");
        set<MongoFile*> temp = mmfiles;
        for ( set<MongoFile*>::iterator i = temp.begin(); i != temp.end(); i++ ) {
            (*i)->close(); // close() now removes from mmfiles
            pm.hit();
        }
        message << "closeAllFiles() finished";
        --closingAllFiles;
    }

    /*static*/ long long MongoFile::totalMappedLength() {
        unsigned long long total = 0;

        LockMongoFilesShared lk;

        for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ )
            total += (*i)->length();

        return total;
    }

    void nullFunc() { }

    // callback notifications
    void (*MongoFile::notifyPreFlush)() = nullFunc;
    void (*MongoFile::notifyPostFlush)() = nullFunc;

    /*static*/ int MongoFile::flushAll( bool sync ) {
        notifyPreFlush();
        int x = _flushAll(sync);
        notifyPostFlush();
        return x;
    }

    /*static*/ int MongoFile::_flushAll( bool sync ) {
        if ( ! sync ) {
            int num = 0;
            LockMongoFilesShared lk;
            for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ) {
                num++;
                MongoFile * mmf = *i;
                if ( ! mmf )
                    continue;

                mmf->flush( sync );
            }
            return num;
        }

        // want to do it sync
        set<MongoFile*> seen;
        while ( true ) {
            auto_ptr<Flushable> f;
            LockMongoFilesShared lk;
            for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ) {
                MongoFile * mmf = *i;
                if ( ! mmf )
                    continue;
                if ( seen.count( mmf ) )
                    continue;
                f.reset( mmf->prepareFlush() );
                seen.insert( mmf );
                break;
            }
            if ( ! f.get() )
                break;

            f->flush();
        }
        return seen.size();
    }

    void MongoFile::created() {
        LockMongoFilesExclusive lk;
        mmfiles.insert(this);
    }

    void MongoFile::setFilename(const std::string& fn) {
        LockMongoFilesExclusive lk;
        verify( _filename.empty() );
        _filename = boost::filesystem::absolute(fn).generic_string();
        MongoFile *&ptf = pathToFile[_filename];
        massert(13617, "MongoFile : multiple opens of same filename", ptf == 0);
        ptf = this;
    }

    MongoFile* MongoFileFinder::findByPath(const std::string& path) const {
        return mapFindWithDefault(pathToFile,
                                  boost::filesystem::absolute(path).generic_string(),
                                  static_cast<MongoFile*>(NULL));
    }


    void printMemInfo( const char * where ) {
        cout << "mem info: ";
        if ( where )
            cout << where << " ";

        ProcessInfo pi;
        if ( ! pi.supported() ) {
            cout << " not supported" << endl;
            return;
        }

        cout << "vsize: " << pi.getVirtualMemorySize() << " resident: " << pi.getResidentSize() << " mapped: " << ( MemoryMappedFile::totalMappedLength() / ( 1024 * 1024 ) ) << endl;
    }

} // namespace mongo
