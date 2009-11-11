// mmap.h

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

#pragma once

namespace mongo {

    class MemoryMappedFile {
    public:

        MemoryMappedFile();
        ~MemoryMappedFile(); /* closes the file if open */
        void close();
        
        // Throws exception if file doesn't exist.
        void* map( const char *filename );

        /* Creates with length if DNE, otherwise uses existing file length,
           passed length.
        */
        void* map(const char *filename, long &length);

        void flush(bool sync);

        void* viewOfs() {
            return view;
        }

        long length() {
            return len;
        }
        
        static void updateLength( const char *filename, long &length );
        
        static long long totalMappedLength();
        static void closeAllFiles( stringstream &message );
        static int flushAll( bool sync );

    private:
        void created();
        
        HANDLE fd;
        HANDLE maphandle;
        void *view;
        long len;
    };
    

} // namespace mongo
