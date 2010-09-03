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

#include "pch.h"
#include "mmap.h"
#include "processinfo.h"
#include "concurrency/rwlock.h"

namespace mongo {

    /*static*/ void MemoryMappedFile::updateLength( const char *filename, long &length ) {
        if ( !boost::filesystem::exists( filename ) )
            return;
        // make sure we map full length if preexisting file.
        boost::uintmax_t l = boost::filesystem::file_size( filename );
        assert( l <= 0x7fffffff );
        length = (long) l;
    }

    void* MemoryMappedFile::map(const char *filename) {
        boost::uintmax_t l = boost::filesystem::file_size( filename );
        assert( l <= 0x7fffffff );
        long i = (long)l;
        return map( filename , i );
    }

    void printMemInfo( const char * where ){
        cout << "mem info: ";
        if ( where ) 
            cout << where << " "; 
        ProcessInfo pi;
        if ( ! pi.supported() ){
            cout << " not supported" << endl;
            return;
        }
        
        cout << "vsize: " << pi.getVirtualMemorySize() << " resident: " << pi.getResidentSize() << " mapped: " << ( MemoryMappedFile::totalMappedLength() / ( 1024 * 1024 ) ) << endl;
    }

    /* --- MongoFile -------------------------------------------------
       this is the administrative stuff 
    */

    static set<MongoFile*> mmfiles;
    static RWLock mmmutex("rw:mmmutex");

    void MongoFile::destroyed() {
        rwlock lk( mmmutex , true );
        mmfiles.erase(this);
    }

    /*static*/
    void MongoFile::closeAllFiles( stringstream &message ) {
        static int closingAllFiles = 0;
        if ( closingAllFiles ) {
            message << "warning closingAllFiles=" << closingAllFiles << endl;
            return;
        }
        ++closingAllFiles;

        rwlock lk( mmmutex , true );

        ProgressMeter pm( mmfiles.size() , 2 , 1 );
        for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
            (*i)->close();
            pm.hit();
        }
        message << "closeAllFiles() finished";
        --closingAllFiles;
    }

    /*static*/ long long MongoFile::totalMappedLength(){
        unsigned long long total = 0;
        
        rwlock lk( mmmutex , false );
        for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ )
            total += (*i)->length();

        return total;
    }

    /*static*/ int MongoFile::flushAll( bool sync ){
        if ( ! sync ){
            int num = 0;
            rwlock lk( mmmutex , false );
            for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
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
        while ( true ){
            auto_ptr<Flushable> f;
            {
                rwlock lk( mmmutex , false );
                for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
                    MongoFile * mmf = *i;
                    if ( ! mmf )
                        continue;
                    if ( seen.count( mmf ) )
                        continue;
                    f.reset( mmf->prepareFlush() );
                    seen.insert( mmf );
                    break;
                }
            }
            if ( ! f.get() )
                break;
            
            f->flush();
        }
        return seen.size();
    }

    void MongoFile::created(){
        rwlock lk( mmmutex , true );
        mmfiles.insert(this);
    }

#ifdef _DEBUG

    void MongoFile::lockAll() {
        rwlock lk( mmmutex , false );
        for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
            MongoFile * mmf = *i;
            if (mmf) mmf->_lock();
        }
    }

    void MongoFile::unlockAll() {
        rwlock lk( mmmutex , false );
        for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
            MongoFile * mmf = *i;
            if (mmf) mmf->_unlock();
        }
    }
#endif

} // namespace mongo
