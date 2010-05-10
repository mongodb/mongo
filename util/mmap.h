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

    /* the administrative-ish stuff here */
    class MongoFile { 
    protected:
        virtual void close() = 0;
        virtual void flush(bool sync) = 0;

        void created(); /* subclass must call after create */
        void destroyed(); /* subclass must call in destructor */
    public:
        virtual long length() = 0;

        enum Options {
            SEQUENTIAL = 1 // hint - e.g. FILE_FLAG_SEQUENTIAL_SCAN on windows
        };

        virtual ~MongoFile() {}

        static int flushAll( bool sync ); // returns n flushed
        static long long totalMappedLength();
        static void closeAllFiles( stringstream &message );

        /* can be "overriden" if necessary */
        static bool exists(boost::filesystem::path p) {
            return boost::filesystem::exists(p);
        }
    };

    class MFTemplate : public MongoFile {
    protected:
        virtual void close();
        virtual void flush(bool sync);
    public:
        virtual long length();

        class Pointer {
        public:
            void* at(int offset, int len);
			void grow(int offset, int len);
            bool isNull() const;
        };

        Pointer map( const char *filename );
        Pointer map(const char *_filename, long &length, int options=0);
    };

    class MemoryMappedFile : public MongoFile {
    public:
        class Pointer {
            char *_base;
        public:
            Pointer() : _base(0) { }
            Pointer(void *p) : _base((char*) p) { }
            void* at(int offset, int maxLen) { return _base + offset; } 
			void grow(int offset, int len) { /* no action required with mem mapped file */ }
            bool isNull() const { return _base == 0; }
        };

        MemoryMappedFile();
        ~MemoryMappedFile() {
            destroyed();
            close();
        }
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

        /*void* viewOfs() {
            return view;
        }*/

        long length() {
            return len;
        }
        
    private:
        static void updateLength( const char *filename, long &length );
        
        HANDLE fd;
        HANDLE maphandle;
        void *view;
        long len;
        string _filename;
    };

    void printMemInfo( const char * where );    

#include "ramstore.h"

//#define _RAMSTORE
#if defined(_RAMSTORE)
    typedef RamStoreFile MMF;
#else
    typedef MemoryMappedFile MMF;
#endif

} // namespace mongo
