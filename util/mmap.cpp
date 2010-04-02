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

#include "stdafx.h"
#include "mmap.h"
#include "processinfo.h"

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
    static mongo::mutex mmmutex;

    MongoFile::~MongoFile() {
        scoped_lock lk( mmmutex );
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
        ProgressMeter pm( mmfiles.size() , 2 , 1 );
        for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
            (*i)->close();
            pm.hit();
        }
        message << "    closeAllFiles() finished" << endl;
        --closingAllFiles;
    }

    /*static*/ long long MongoFile::totalMappedLength(){
        unsigned long long total = 0;
        
        scoped_lock lk( mmmutex );
        for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ )
            total += (*i)->length();

        return total;
    }

    /*static*/ int MongoFile::flushAll( bool sync ){
        int num = 0;

        scoped_lock lk( mmmutex );
        for ( set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
            num++;
            MongoFile * mmf = *i;
            if ( ! mmf )
                continue;
            mmf->flush( sync );
        }
        return num;
    }

    void MongoFile::created(){
        scoped_lock lk( mmmutex );
        mmfiles.insert(this);
    }

} // namespace mongo
