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
        
        virtual ~MongoFile() {}

        enum Options {
            SEQUENTIAL = 1, // hint - e.g. FILE_FLAG_SEQUENTIAL_SCAN on windows
            READONLY = 2    // not contractually guaranteed, but if specified the impl has option to fault writes
        };

        static int flushAll( bool sync ); // returns n flushed
        static long long totalMappedLength();
        static void closeAllFiles( stringstream &message );

        // Locking allows writes. Reads are always allowed
        static void lockAll();
        static void unlockAll();
        static bool exists(boost::filesystem::path p) { return boost::filesystem::exists(p); }

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

        virtual unsigned long long length() const = 0;

        // only supporting on posix mmap
        virtual void _lock() {}
        virtual void _unlock() {}
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

    class MemoryMappedFile : public MongoFile {
    public:
        MemoryMappedFile();

        virtual ~MemoryMappedFile() {
            destroyed(); // cleans up from the master list of mmaps
            close();
        }

        virtual void close();

        // Throws exception if file doesn't exist. (dm may2010: not sure if this is always true?)
        void* map(const char *filename);
        void* mapWithOptions(const char *filename, int options);

        /* Creates with length if DNE, otherwise uses existing file length,
           passed length.
           @param options MongoFile::Options bits
        */
        void* map(const char *filename, unsigned long long &length, int options = 0 );

        /* Create. Must not exist. 
           @param zero fill file with zeros when true
        */
        void* create(string filename, unsigned long long len, bool zero);

        void flush(bool sync);
        virtual Flushable * prepareFlush();

        long shortLength() const          { return (long) len; }
        unsigned long long length() const { return len; }
        string filename() const           { return _filename; }

        /** create a new view with the specified properties. 
            automatically cleaned up upon close/destruction of the MemoryMappedFile object. 
            */
        void* createReadOnlyMap();
        void* createPrivateMap();

    private:
        static void updateLength( const char *filename, unsigned long long &length );
        
        HANDLE fd;
        HANDLE maphandle;
        vector<void *> views;
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

    typedef MemoryMappedFile MMF;

} // namespace mongo
