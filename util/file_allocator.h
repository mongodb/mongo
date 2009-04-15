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

#include "stdafx.h"
#include <fcntl.h>

#ifndef O_NOATIME
#define O_NOATIME 0
#endif

namespace mongo {

    // Handles allocation of contiguous files on disk.
    class FileAllocator {
        // The public functions may not be called concurrently.  If
        // allocateAsap() is called for a file after requestAllocation(), the
        // sizes in each call must be the same.
    public:
        void start() {
            Runner r( *this );
            boost::thread t( r );
        }
        // May be called if file exists, but may not be called more than once
        // for a file.
        void requestAllocation( const string &name, int size ) {
            if ( boost::filesystem::exists( name ) )
                return;
            {
                boostlock lk( pendingMutex_ );
                pending_.push_back( make_pair( name, size ) );
            }
            pendingUpdated_.notify_all();
        }
        // Returns when file has been allocated.
        void allocateAsap( const string &name, int size ) {
            pair< string, int > spec( name, size );
            {
                boostlock lk( pendingMutex_ );
                if ( allocated( name ) )
                    return;
                if ( pending_.size() == 0 )
                    pending_.push_back( spec );
                else if ( pending_.front() != spec ) {
                    pending_.remove( spec );
                    list< pair< string, int > >::iterator i = pending_.begin();
                    ++i;
                    pending_.insert( i, spec );
                }
            }
            pendingUpdated_.notify_all();
            boostlock lk( pendingMutex_ );
            while( 1 ) {
                if ( allocated( name ) ) {
                    return;
                }
                pendingUpdated_.wait( lk );                    
            }
        }

        void waitUntilFinished() const {
            boostlock lk( pendingMutex_ );
            while( 1 ) {
                if ( pending_.size() == 0 )
                    return;
                pendingUpdated_.wait( lk );
            }
        }
        
    private:
        // caller must hold pendingMutex_ lock
        bool allocated( const string &name ) const {
            if ( !boost::filesystem::exists( name ) )
                return false;
            for( list< pair< string, int > >::const_iterator i = pending_.begin(); i != pending_.end(); ++i )
                if ( i->first == name )
                    return false;
            return true;
        }

        mutable boost::mutex pendingMutex_;
        mutable boost::condition_variable pendingUpdated_;
        list< pair< string, int > > pending_;
        
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
                        int size;
                        {
                            boostlock lk( a_.pendingMutex_ );
                            if ( a_.pending_.size() == 0 )
                                break;
                            name = a_.pending_.front().first;
                            size = a_.pending_.front().second;
                        }
                        try {
                            int fd = open(name.c_str(), O_CREAT | O_RDWR | O_NOATIME, S_IRUSR | S_IWUSR);
                            if ( fd <= 0 ) {
                                stringstream ss;
                                ss << "couldn't open " << name << ' ' << errno;
                                massert( ss.str(), fd <= 0 );
                            }
                            
                            /* make sure the file is the full desired length */
                            off_t filelen = lseek(fd, 0, SEEK_END);
                            if ( filelen < size ) {
                                massert( "failure mapping new file", filelen == 0 );
                                // Check for end of disk.
                                massert( "Unable to allocate file of desired size",
                                        size - 1 == lseek(fd, size - 1, SEEK_SET) );
                                massert( "Unable to allocate file of desired size",
                                        1 == write(fd, "", 1) );
                                lseek(fd, 0, SEEK_SET);
                                log() << "allocating new datafile " << name << ", filling with zeroes..." << endl;
                                Timer t;
                                int z = 8192;
                                char buf[z];
                                memset(buf, 0, z);
                                int left = size;
                                while ( 1 ) {
                                    if ( left <= z ) {
                                        write(fd, buf, left);
                                        break;
                                    }
                                    write(fd, buf, z);
                                    left -= z;
                                }
                                log() << "done allocating datafile " << name << ", size: " << size << ", took " << ((double)t.millis())/1000.0 << " secs" << endl;
                            }                            
                            close( fd );
                            
                        } catch ( ... ) {
                            problem() << "Failed to allocate new file: " << name
                                      << ", size: " << size << ", aborting." << endl;
                        }
                        
                        {
                            boostlock lk( a_.pendingMutex_ );
                            a_.pending_.pop_front();
                        }
                        a_.pendingUpdated_.notify_all();
                    }
                }
            }
        };
    };
    
    FileAllocator &theFileAllocator();
    
} // namespace mongo