// @file file_allocator.cpp

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

#include "mongo/util/file_allocator.h"

#include <boost/thread.hpp>
#include <boost/filesystem/operations.hpp>
#include <errno.h>
#include <fcntl.h>

#if defined(__freebsd__) || defined(__openbsd__)
#   include <sys/stat.h>
#endif

#if defined(__linux__)
#   include <sys/vfs.h>
#endif

#if defined(_WIN32)
#   include <io.h>
#endif

#include "mongo/platform/posix_fadvise.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/paths.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

using namespace mongoutils;

#ifndef O_NOATIME
#define O_NOATIME (0)
#endif

namespace mongo {

    // unique number for temporary file names
    unsigned long long FileAllocator::_uniqueNumber = 0;
    static SimpleMutex _uniqueNumberMutex( "uniqueNumberMutex" );

    /**
     * Aliases for Win32 CRT functions
     */
#if defined(_WIN32)
    static inline long lseek(int fd, long offset, int origin) { return _lseek(fd, offset, origin); }
    static inline int write(int fd, const void *data, int count) { return _write(fd, data, count); }
    static inline int close(int fd) { return _close(fd); }
#endif

    boost::filesystem::path ensureParentDirCreated(const boost::filesystem::path& p){
        const boost::filesystem::path parent = p.branch_path();

        if (! boost::filesystem::exists(parent)){
            ensureParentDirCreated(parent);
            log() << "creating directory " << parent.string() << endl;
            boost::filesystem::create_directory(parent);
            flushMyDirectory(parent); // flushes grandparent to ensure parent exists after crash
        }
        
        verify(boost::filesystem::is_directory(parent));
        return parent;
    }

    FileAllocator::FileAllocator()
        : _pendingMutex("FileAllocator"), _failed() {
    }


    void FileAllocator::start() {
        boost::thread t( boost::bind( &FileAllocator::run , this ) );
    }

    void FileAllocator::requestAllocation( const string &name, long &size ) {
        scoped_lock lk( _pendingMutex );
        if ( _failed )
            return;
        long oldSize = prevSize( name );
        if ( oldSize != -1 ) {
            size = oldSize;
            return;
        }
        _pending.push_back( name );
        _pendingSize[ name ] = size;
        _pendingUpdated.notify_all();
    }

    void FileAllocator::allocateAsap( const string &name, unsigned long long &size ) {
        scoped_lock lk( _pendingMutex );
        long oldSize = prevSize( name );
        if ( oldSize != -1 ) {
            size = oldSize;
            if ( !inProgress( name ) )
                return;
        }
        checkFailure();
        _pendingSize[ name ] = size;
        if ( _pending.size() == 0 )
            _pending.push_back( name );
        else if ( _pending.front() != name ) {
            _pending.remove( name );
            list< string >::iterator i = _pending.begin();
            ++i;
            _pending.insert( i, name );
        }
        _pendingUpdated.notify_all();
        while( inProgress( name ) ) {
            checkFailure();
            _pendingUpdated.wait( lk.boost() );
        }

    }

    void FileAllocator::waitUntilFinished() const {
        if ( _failed )
            return;
        scoped_lock lk( _pendingMutex );
        while( _pending.size() != 0 )
            _pendingUpdated.wait( lk.boost() );
    }

    // TODO: pull this out to per-OS files once they exist
    static bool useSparseFiles(int fd) {
#if defined(__linux__)
// these are from <linux/magic.h> but that isn't available on all systems
# define NFS_SUPER_MAGIC 0x6969

        struct statfs fs_stats;
        int ret = fstatfs(fd, &fs_stats);
        uassert(16062, "fstatfs failed: " + errnoWithDescription(), ret == 0);

        return (fs_stats.f_type == NFS_SUPER_MAGIC);

#elif defined(__freebsd__) || defined(__sunos__)
        // assume using ZFS which is copy-on-write so no benefit to zero-filling
        // TODO: check which fs we are using like we do on linux
        return true;
#else
        return false;
#endif
    }

    void FileAllocator::ensureLength(int fd , long size) {
#if !defined(_WIN32)
        if (useSparseFiles(fd)) {
            LOG(1) << "using ftruncate to create a sparse file" << endl;
            int ret = ftruncate(fd, size);
            uassert(16063, "ftruncate failed: " + errnoWithDescription(), ret == 0);
            return;
        }
#endif

#if defined(__linux__)
        int ret = posix_fallocate(fd,0,size);
        if ( ret == 0 )
            return;

        log() << "FileAllocator: posix_fallocate failed: " << errnoWithDescription( ret ) << " falling back" << endl;
#endif

        off_t filelen = lseek( fd, 0, SEEK_END );
        if ( filelen < size ) {
            if (filelen != 0) {
                stringstream ss;
                ss << "failure creating new datafile; lseek failed for fd " << fd << " with errno: " << errnoWithDescription();
                uassert( 10440 ,  ss.str(), filelen == 0 );
            }
            // Check for end of disk.

            uassert( 10441 ,  str::stream() << "Unable to allocate new file of size " << size << ' ' << errnoWithDescription(),
                     size - 1 == lseek(fd, size - 1, SEEK_SET) );
            uassert( 10442 ,  str::stream() << "Unable to allocate new file of size " << size << ' ' << errnoWithDescription(),
                     1 == write(fd, "", 1) );
            lseek(fd, 0, SEEK_SET);

            const long z = 256 * 1024;
            const boost::scoped_array<char> buf_holder (new char[z]);
            char* buf = buf_holder.get();
            memset(buf, 0, z);
            long left = size;
            while ( left > 0 ) {
                long towrite = left;
                if ( towrite > z )
                    towrite = z;

                int written = write( fd , buf , towrite );
                uassert( 10443 , errnoWithPrefix("FileAllocator: file write failed" ), written > 0 );
                left -= written;
            }
        }
    }

    bool FileAllocator::hasFailed() const {
        return _failed;
    }

    void FileAllocator::checkFailure() {
        if (_failed) {
            // we want to log the problem (diskfull.js expects it) but we do not want to dump a stack tracke
            msgassertedNoTrace( 12520, "new file allocation failure" );
        }
    }

    long FileAllocator::prevSize( const string &name ) const {
        if ( _pendingSize.count( name ) > 0 )
            return _pendingSize[ name ];
        if ( boost::filesystem::exists( name ) )
            return boost::filesystem::file_size( name );
        return -1;
    }

    // caller must hold _pendingMutex lock.
    bool FileAllocator::inProgress( const string &name ) const {
        for( list< string >::const_iterator i = _pending.begin(); i != _pending.end(); ++i )
            if ( *i == name )
                return true;
        return false;
    }

    string FileAllocator::makeTempFileName( boost::filesystem::path root ) {
        while( 1 ) {
            boost::filesystem::path p = root / "_tmp";
            stringstream ss;
            unsigned long long thisUniqueNumber;
            {
                // increment temporary file name counter
                // TODO: SERVER-6055 -- Unify temporary file name selection
                SimpleMutex::scoped_lock lk(_uniqueNumberMutex);
                thisUniqueNumber = _uniqueNumber;
                ++_uniqueNumber;
            }
            ss << thisUniqueNumber;
            p /= ss.str();
            string fn = p.string();
            if( !boost::filesystem::exists(p) )
              return fn;
        }
        return "";
	}

    void FileAllocator::run( FileAllocator * fa ) {
        setThreadName( "FileAllocator" );
        {
            // initialize unique temporary file name counter
            // TODO: SERVER-6055 -- Unify temporary file name selection
            SimpleMutex::scoped_lock lk(_uniqueNumberMutex);
            _uniqueNumber = curTimeMicros64();
        }
        while( 1 ) {
            {
                scoped_lock lk( fa->_pendingMutex );
                if ( fa->_pending.size() == 0 )
                    fa->_pendingUpdated.wait( lk.boost() );
            }
            while( 1 ) {
                string name;
                long size;
                {
                    scoped_lock lk( fa->_pendingMutex );
                    if ( fa->_pending.size() == 0 )
                        break;
                    name = fa->_pending.front();
                    size = fa->_pendingSize[ name ];
                }

                string tmp;
                long fd = 0;
                try {
                    log() << "allocating new datafile " << name << ", filling with zeroes..." << endl;
                    
                    boost::filesystem::path parent = ensureParentDirCreated(name);
                    tmp = fa->makeTempFileName( parent );
                    ensureParentDirCreated(tmp);

#if defined(_WIN32)
                    fd = _open( tmp.c_str(), _O_RDWR | _O_CREAT | O_NOATIME, _S_IREAD | _S_IWRITE );
#else
                    fd = open(tmp.c_str(), O_CREAT | O_RDWR | O_NOATIME, S_IRUSR | S_IWUSR);
#endif
                    if ( fd < 0 ) {
                        log() << "FileAllocator: couldn't create " << name << " (" << tmp << ") " << errnoWithDescription() << endl;
                        uasserted(10439, "");
                    }

#if defined(POSIX_FADV_DONTNEED)
                    if( posix_fadvise(fd, 0, size, POSIX_FADV_DONTNEED) ) {
                        log() << "warning: posix_fadvise fails " << name << " (" << tmp << ") " << errnoWithDescription() << endl;
                    }
#endif

                    Timer t;

                    /* make sure the file is the full desired length */
                    ensureLength( fd , size );

                    close( fd );
                    fd = 0;

                    if( rename(tmp.c_str(), name.c_str()) ) {
                        const string& errStr = errnoWithDescription();
                        const string& errMessage = str::stream()
                                << "error: couldn't rename " << tmp
                                << " to " << name << ' ' << errStr;
                        msgasserted(13653, errMessage);
                    }
                    flushMyDirectory(name);

                    log() << "done allocating datafile " << name << ", "
                          << "size: " << size/1024/1024 << "MB, "
                          << " took " << ((double)t.millis())/1000.0 << " secs"
                          << endl;

                    // no longer in a failed state. allow new writers.
                    fa->_failed = false;
                }
                catch ( const std::exception& e ) {
                    log() << "error: failed to allocate new file: " << name
                          << " size: " << size << ' ' << e.what()
                          << ".  will try again in 10 seconds" << endl;
                    if ( fd > 0 )
                        close( fd );
                    try {
                        if ( ! tmp.empty() )
                            boost::filesystem::remove( tmp );
                        boost::filesystem::remove( name );
                    } catch ( const std::exception& e ) {
                        log() << "error removing files: " << e.what() << endl;
                    }
                    scoped_lock lk( fa->_pendingMutex );
                    fa->_failed = true;
                    // not erasing from pending
                    fa->_pendingUpdated.notify_all();
                    
                    
                    sleepsecs(10);
                    continue;
                }

                {
                    scoped_lock lk( fa->_pendingMutex );
                    fa->_pendingSize.erase( name );
                    fa->_pending.pop_front();
                    fa->_pendingUpdated.notify_all();
                }
            }
        }
    }

    FileAllocator* FileAllocator::_instance = 0;

    FileAllocator* FileAllocator::get(){
        if ( ! _instance )
            _instance = new FileAllocator();
        return _instance;
    }

} // namespace mongo
