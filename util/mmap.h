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

    class MemoryMappedFile2 {
    public:
        class Pointer {
        public:
            void* at(int offset);
        };

        // throws exception if file doesn't exist.
        Pointer map( const char *filename );
        Pointer map(const char *_filename, long &length, int options=0);

        long length();
    };

    class MemoryMappedFile {
    public:
        class Pointer {
            char *_base;
        public:
            Pointer() : _base(0) { }
            Pointer(void *p) : _base((char*) p) { }
            void* at(int offset) { return _base + offset; } 
            bool isNull() const { return _base == 0; }
        };

        enum Options {
            SEQUENTIAL = 1 // hint - like FILE_FLAG_SEQUENTIAL_SCAN on windows
        };

        MemoryMappedFile();
        ~MemoryMappedFile(); /* closes the file if open */
        void close();
        
        // Throws exception if file doesn't exist. (dm may2010: not sure if this is always true?)
        void* map( const char *filename );
        /*Pointer pmap( const char *filename ) {
            void *p = map(filename);
            uassert(13077, "couldn't open/map file", p);
            return Pointer(p);
        }*/

        /* Creates with length if DNE, otherwise uses existing file length,
           passed length.
        */
        void* map(const char *filename, long &length, int options = 0 );

        void flush(bool sync);

        void* viewOfs() {
            return view;
        }

        long length() {
            return len;
        }
        
        static long long totalMappedLength();
        static void closeAllFiles( stringstream &message );
        static int flushAll( bool sync ); // returns n flushed

    private:
        static void updateLength( const char *filename, long &length );
        void created();
        
        HANDLE fd;
        HANDLE maphandle;
        void *view;
        long len;
    };

    void printMemInfo( const char * where );    

} // namespace mongo
