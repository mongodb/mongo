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
    class MongoFile : boost::noncopyable { 
        
    public:
        /** Flushable has to fail nicely if the underlying object gets killed */
        class Flushable {
        public:
            virtual ~Flushable(){}
            virtual void flush() = 0;
        };
        
    protected:
        virtual void close() = 0;
        virtual void flush(bool sync) = 0;
        /**
         * returns a thread safe object that you can call flush on
         * Flushable has to fail nicely if the underlying object gets killed
         */
        virtual Flushable * prepareFlush() = 0;
        
        void created(); /* subclass must call after create */
        void destroyed(); /* subclass must call in destructor */

        // only supporting on posix mmap
        virtual void _lock() {}
        virtual void _unlock() {}

    public:
        virtual ~MongoFile() {}
        virtual unsigned long long length() const = 0;

        enum Options {
            SEQUENTIAL = 1 // hint - e.g. FILE_FLAG_SEQUENTIAL_SCAN on windows
        };

        static int flushAll( bool sync ); // returns n flushed
        static long long totalMappedLength();
        static void closeAllFiles( stringstream &message );

        // Locking allows writes. Reads are always allowed
        static void lockAll();
        static void unlockAll();

        /* can be "overriden" if necessary */
        static bool exists(boost::filesystem::path p) {
            return boost::filesystem::exists(p);
        }
    };

#ifndef _DEBUG
    // no-ops in production
    inline void MongoFile::lockAll() {}
    inline void MongoFile::unlockAll() {}

#endif

    struct MongoFileAllowWrites {
        MongoFileAllowWrites(){
            MongoFile::lockAll();
        }
        ~MongoFileAllowWrites(){
            MongoFile::unlockAll();
        }
    };

    /** template for what a new storage engine's class definition must implement 
        PRELIMINARY - subject to change.
    */
    class StorageContainerTemplate : public MongoFile {
    protected:
        virtual void close();
        virtual void flush(bool sync);
    public:
        virtual long length();

        /** pointer to a range of space in this storage unit */
        class Pointer {
        public:
            /** retried address of buffer at offset 'offset' withing the storage unit. returned range is a contiguous 
                buffer reflecting what is in storage.  caller will not read or write past 'len'.

                note calls may be received that are at different points in a range and different lengths. however 
                for now assume that on writes, if a call is made, previously returned addresses are no longer valid. i.e.
                  p = at(10000, 500);
                  q = at(10000, 600);
                after the second call it is ok if p is invalid.
            */
            void* at(int offset, int len);

            void* atAsIndicated(int offset);

            /** indicate that we wrote to the range (from a previous at() call) and that it needs 
                flushing to disk.
                */
            void written(int offset, int len);

            bool isNull() const;
        };

        /** commit written() calls from above. */
        void commit();
        
        Pointer open(const char *filename);
        Pointer open(const char *_filename, long &length, int options=0);
    };

    class MemoryMappedFile : public MongoFile {
    public:
        class Pointer {
            char *_base;
        public:
            Pointer() : _base(0) { }
            Pointer(void *p) : _base((char*) p) { }
            void* at(int offset, int maxLen) { return _base + offset; } 
            void* atAsIndicated(int offset) { return _base + offset; } 
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

        /*To replace map():
        
          Pointer open( const char *filename ) {
            void *p = map(filename);
            uassert(13077, "couldn't open/map file", p);
            return Pointer(p);
        }*/

        /* Creates with length if DNE, otherwise uses existing file length,
           passed length.
        */
        void* map(const char *filename, unsigned long long &length, int options = 0 );

        /* Create. Must not exist. 
           @param zero fill file with zeros when true
        */
        void* create(string filename, unsigned long long len, bool zero);

        void flush(bool sync);
        virtual Flushable * prepareFlush();

        /*void* viewOfs() {
            return view;
        }*/

        long shortLength() const { return (long) len; }
        unsigned long long length() const { return len; }

        string filename() const { return _filename; }

    private:
        static void updateLength( const char *filename, unsigned long long &length );
        
        HANDLE fd;
        HANDLE maphandle;
        void *view;
        unsigned long long len;
        string _filename;

#ifdef _WIN32
        boost::shared_ptr<mutex> _flushMutex;
#endif

    protected:
        // only posix mmap implementations will support this
        virtual void _lock();
        virtual void _unlock();

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
