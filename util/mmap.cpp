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

namespace mongo {

    set<MemoryMappedFile*> mmfiles;
    boost::mutex mmmutex;

    MemoryMappedFile::~MemoryMappedFile() {
        close();
        boostlock lk( mmmutex );
        mmfiles.erase(this);
    }

    void MemoryMappedFile::created(){
        boostlock lk( mmmutex );
        mmfiles.insert(this);
    }

    /*static*/
    int closingAllFiles = 0;
    void MemoryMappedFile::closeAllFiles( stringstream &message ) {
        if ( closingAllFiles ) {
            message << "warning closingAllFiles=" << closingAllFiles << endl;
            return;
        }
        ++closingAllFiles;
        ProgressMeter pm( mmfiles.size() , 2 , 1 );
        for ( set<MemoryMappedFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
            (*i)->close();
            pm.hit();
        }
        message << "    closeAllFiles() finished" << endl;
        --closingAllFiles;
    }

    long long MemoryMappedFile::totalMappedLength(){
        unsigned long long total = 0;
        
        boostlock lk( mmmutex );
        for ( set<MemoryMappedFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ )
            total += (*i)->length();

        return total;
    }

    int MemoryMappedFile::flushAll( bool sync ){
        int num = 0;

        boostlock lk( mmmutex );
        for ( set<MemoryMappedFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++ ){
            num++;
            MemoryMappedFile * mmf = *i;
            if ( ! mmf )
                continue;
            mmf->flush( sync );
        }
        return num;
    }


    void MemoryMappedFile::updateLength( const char *filename, long &length ) {
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

} // namespace mongo
