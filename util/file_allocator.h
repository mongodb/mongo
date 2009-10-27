//file_allocator.h

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

#include "../stdafx.h"
#include <fcntl.h>
#include <errno.h>
#if defined(__freebsd__)
#include <sys/stat.h>
#endif

#ifndef O_NOATIME
#define O_NOATIME 0
#endif

namespace mongo {

    /* Handles allocation of contiguous files on disk.  Allocation may be
       requested asynchronously or synchronously.
       */
    class FileAllocator {
        /* The public functions may not be called concurrently.  The allocation
           functions may be called multiple times per file, but only the first
           size specified per file will be used.
        */
    public:
#if !defined(_WIN32)
        FileAllocator() : failed_() {}
#endif
        void start() {
#if !defined(_WIN32)
            Runner r( *this );
            boost::thread t( r );
#endif
        }
        // May be called if file exists. If file exists, or its allocation has
        // been requested, size is updated to match existing file size.
        void requestAllocation( const string &name, long &size ) {
#if !defined(_WIN32)
            boostlock lk( pendingMutex_ );
            long oldSize = prevSize( name );
            if ( oldSize != -1 ) {
                size = oldSize;
                return;
            }
            pending_.push_back( name );
            pendingSize_[ name ] = size;
            pendingUpdated_.notify_all();
#endif
        }
        // Returns when file has been allocated.  If file exists, size is
        // updated to match existing file size.
        void allocateAsap( const string &name, long &size ) {
#if !defined(_WIN32)
            boostlock lk( pendingMutex_ );
            long oldSize = prevSize( name );
            if ( oldSize != -1 ) {
                size = oldSize;
                if ( !inProgress( name ) )
                    return;
            }
            pendingSize_[ name ] = size;
            if ( pending_.size() == 0 )
                pending_.push_back( name );
            else if ( pending_.front() != name ) {
                pending_.remove( name );
                list< string >::iterator i = pending_.begin();
                ++i;
                pending_.insert( i, name );
            }
            pendingUpdated_.notify_all();
            while( inProgress( name ) )
                pendingUpdated_.wait( lk );
#endif
        }

        void waitUntilFinished() const {
#if !defined(_WIN32)
            if ( failed_ )
                return;
            boostlock lk( pendingMutex_ );
            while( pending_.size() != 0 )
                pendingUpdated_.wait( lk );
#endif
        }
        
    private:
#if !defined(_WIN32)
        // caller must hold pendingMutex_ lock.  Returns size if allocated or 
        // allocation requested, -1 otherwise.
        long prevSize( const string &name ) const {
            if ( pendingSize_.count( name ) > 0 )
                return pendingSize_[ name ];
            if ( boost::filesystem::exists( name ) )
                return boost::filesystem::file_size( name );
            return -1;
        }
         
        // caller must hold pendingMutex_ lock.
        bool inProgress( const string &name ) const {
            for( list< string >::const_iterator i = pending_.begin(); i != pending_.end(); ++i )
                if ( *i == name )
                    return true;
            return false;
        }

        mutable boost::mutex pendingMutex_;
        mutable boost::condition pendingUpdated_;
        list< string > pending_;
        mutable map< string, long > pendingSize_;
        bool failed_;
        
        struct Runner {
            Runner( FileAllocator &allocator ) : a_( allocator ) {}
            FileAllocator &a_;
            void operator()() {
                while( 1 ) {
                    {
                        boostlock lk( a_.pendingMutex_ );
                        if ( a_.pending_.size() == 0 )
                            a_.pendingUpdated_.wait( lk );
                    }
                    while( 1 ) {
                        string name;
                        long size;
                        {
                            boostlock lk( a_.pendingMutex_ );
                            if ( a_.pending_.size() == 0 )
                                break;
                            name = a_.pending_.front();
                            size = a_.pendingSize_[ name ];
                        }
                        try {
                            long fd = open(name.c_str(), O_CREAT | O_RDWR | O_NOATIME, S_IRUSR | S_IWUSR);
                            if ( fd <= 0 ) {
                                stringstream ss;
                                ss << "couldn't open " << name << ' ' << errno;
                                massert( ss.str(), fd <= 0 );
                            }

#if defined(POSIX_FADV_DONTNEED)
                            if( posix_fadvise(fd, 0, size, POSIX_FADV_DONTNEED) ) { 
                                log() << "warning: posix_fadvise fails " << name << ' ' << errno << endl;
                            }
#endif
  
                            /* make sure the file is the full desired length */
                            off_t filelen = lseek(fd, 0, SEEK_END);
                            if ( filelen < size ) {
                                massert( "failure creating new datafile", filelen == 0 );
                                // Check for end of disk.
                                massert( "Unable to allocate file of desired size",
                                        size - 1 == lseek(fd, size - 1, SEEK_SET) );
                                massert( "Unable to allocate file of desired size",
                                        1 == write(fd, "", 1) );
                                lseek(fd, 0, SEEK_SET);
                                log() << "allocating new datafile " << name << ", filling with zeroes..." << endl;
                                Timer t;
                                long z = 256 * 1024;
                                char buf[z];
                                memset(buf, 0, z);
                                long left = size;
                                while ( 1 ) {
                                    if ( left <= z ) {
                                        massert( "write failed", left == write(fd, buf, left) );
                                        break;
                                    }
                                    massert( "write failed", z == write(fd, buf, z) );
                                    left -= z;
                                }
                                log() << "done allocating datafile " << name << ", size: " << size/1024/1024 << "MB, took " << ((double)t.millis())/1000.0 << " secs" << endl;
                            }                            
                            close( fd );
                            
                        } catch ( ... ) {
                            problem() << "Failed to allocate new file: " << name
                                      << ", size: " << size << ", aborting." << endl;
                            try {
                                BOOST_CHECK_EXCEPTION( boost::filesystem::remove( name ) );
                            } catch ( ... ) {
                            }
                            a_.failed_ = true;
                            dbexit( EXIT_FS );
                        }
                        
                        {
                            boostlock lk( a_.pendingMutex_ );
                            a_.pendingSize_.erase( name );
                            a_.pending_.pop_front();
                            a_.pendingUpdated_.notify_all();
                        }
                    }
                }
            }
        };
#endif    
    };
    
    FileAllocator &theFileAllocator();
} // namespace mongo
