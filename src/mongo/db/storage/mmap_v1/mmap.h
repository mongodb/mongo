// mmap.h

/*    Copyright 2009 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <set>
#include <sstream>
#include <vector>

#include "mongo/util/concurrency/rwlock.h"

namespace mongo {

#if !defined(_WIN32)
typedef int HANDLE;
#endif

extern const size_t g_minOSPageSizeBytes;
void minOSPageSizeBytesTest(size_t minOSPageSizeBytes);  // lame-o

// call this if syncing data fails
void dataSyncFailedHandler();

class MAdvise {
    MONGO_DISALLOW_COPYING(MAdvise);

public:
    enum Advice { Sequential = 1, Random = 2 };
    MAdvise(void* p, unsigned len, Advice a);
    ~MAdvise();  // destructor resets the range to MADV_NORMAL
private:
    void* _p;
    unsigned _len;
};

// lock order: lock dbMutex before this if you lock both
class LockMongoFilesShared {
    friend class LockMongoFilesExclusive;
    static RWLockRecursiveNongreedy mmmutex;
    static unsigned era;
    RWLockRecursive::Shared lk;

public:
    LockMongoFilesShared() : lk(mmmutex) {}

    /** era changes anytime memory maps come and go.  thus you can use this as a cheap way to check
        if nothing has changed since the last time you locked.  Of course you must be shared locked
        at the time of this call, otherwise someone could be in progress.

        This is used for yielding; see PageFaultException::touch().
    */
    static unsigned getEra() {
        return era;
    }

    static void assertExclusivelyLocked() {
        mmmutex.assertExclusivelyLocked();
    }
    static void assertAtLeastReadLocked() {
        mmmutex.assertAtLeastReadLocked();
    }
};

class LockMongoFilesExclusive {
    RWLockRecursive::Exclusive lk;

public:
    LockMongoFilesExclusive() : lk(LockMongoFilesShared::mmmutex) {
        LockMongoFilesShared::era++;
    }
};

/* the administrative-ish stuff here */
class MongoFile {
    MONGO_DISALLOW_COPYING(MongoFile);

public:
    /** Flushable has to fail nicely if the underlying object gets killed */
    class Flushable {
    public:
        virtual ~Flushable() {}
        virtual void flush() = 0;
    };

    enum Options {
        NONE = 0,
        SEQUENTIAL = 1 << 0,  // hint - e.g. FILE_FLAG_SEQUENTIAL_SCAN on windows.
        READONLY = 1 << 1     // if true, writing to the mapped file will crash the process.
    };

    // Integral type used as a BitSet of Options.
    using OptionSet = std::underlying_type<Options>::type;

    MongoFile(OptionSet options);
    virtual ~MongoFile() = default;

    /** @param fun is called for each MongoFile.
        called from within a mutex that MongoFile uses. so be careful not to deadlock.
    */
    template <class F>
    static void forEach(F fun);

    /**
     * note: you need to be in mmmutex when using this. forEach (above) handles that for you
     * automatically.
     */
    static std::set<MongoFile*>& getAllFiles();

    static int flushAll(bool sync);  // returns n flushed
    static long long totalMappedLength();
    static void closeAllFiles(std::stringstream& message);

    virtual bool isDurableMappedFile() {
        return false;
    }

    std::string filename() const {
        return _filename;
    }
    void setFilename(const std::string& fn);

    virtual uint64_t getUniqueId() const = 0;

private:
    std::string _filename;
    static int _flushAll(bool sync);  // returns n flushed
    const OptionSet _options;

protected:
    virtual void close() = 0;
    virtual void flush(bool sync) = 0;
    /**
     * returns a thread safe object that you can call flush on
     * Flushable has to fail nicely if the underlying object gets killed
     */
    virtual Flushable* prepareFlush() = 0;

    void created(); /* subclass must call after create */

    /* subclass must call in destructor (or at close).
       removes this from pathToFile and other maps
       safe to call more than once, albeit might be wasted work
       ideal to call close to the close, if the close is well before object destruction
    */
    void destroyed();

    virtual unsigned long long length() const = 0;

    bool isOptionSet(Options option) const {
        return _options & option;
    }
};

/** look up a MMF by filename. scoped mutex locking convention.
    example:
      MMFFinderByName finder;
      DurableMappedFile *a = finder.find("file_name_a");
      DurableMappedFile *b = finder.find("file_name_b");
*/
class MongoFileFinder {
    MONGO_DISALLOW_COPYING(MongoFileFinder);

public:
    MongoFileFinder() {}

    /** @return The MongoFile object associated with the specified file name.  If no file is open
                with the specified name, returns null.
    */
    MongoFile* findByPath(const std::string& path) const;

private:
    LockMongoFilesShared _lk;
};

class MemoryMappedFile : public MongoFile {
protected:
    virtual void* viewForFlushing() {
        if (views.size() == 0)
            return 0;
        verify(views.size() == 1);
        return views[0];
    }

public:
    MemoryMappedFile(OptionSet options = NONE);

    virtual ~MemoryMappedFile() {
        LockMongoFilesExclusive lk;
        close();
    }

    virtual void close();

    // Throws exception if file doesn't exist. (dm may2010: not sure if this is always true?)
    void* map(const char* filename);

    /* Creates with length if DNE, otherwise uses existing file length,
       passed length.
    */
    void* map(const char* filename, unsigned long long& length);

    /* Create. Must not exist.
       @param zero fill file with zeros when true
    */
    void* create(const std::string& filename, unsigned long long len, bool zero);

    void flush(bool sync);
    virtual Flushable* prepareFlush();

    long shortLength() const {
        return (long)len;
    }
    unsigned long long length() const {
        return len;
    }
    HANDLE getFd() const {
        return fd;
    }
    /** create a new view with the specified properties.
        automatically cleaned up upon close/destruction of the MemoryMappedFile object.
        */
    void* createPrivateMap();

    virtual uint64_t getUniqueId() const {
        return _uniqueId;
    }

private:
    static void updateLength(const char* filename, unsigned long long& length);

    HANDLE fd = 0;
    HANDLE maphandle = 0;
    std::vector<void*> views;
    unsigned long long len = 0u;
    const uint64_t _uniqueId;
#ifdef _WIN32
    // flush Mutex
    //
    // Protects:
    //  Prevent flush() and close() from concurrently running.
    //  It ensures close() cannot complete while flush() is running
    // Lock Ordering:
    //  LockMongoFilesShared must be taken before _flushMutex if both are taken
    stdx::mutex _flushMutex;
#endif

protected:
    /** close the current private view and open a new replacement */
    void* remapPrivateView(void* oldPrivateAddr);
};

/** p is called from within a mutex that MongoFile uses.  so be careful not to deadlock. */
template <class F>
inline void MongoFile::forEach(F p) {
    LockMongoFilesShared lklk;
    const std::set<MongoFile*>& mmfiles = MongoFile::getAllFiles();
    for (std::set<MongoFile*>::const_iterator i = mmfiles.begin(); i != mmfiles.end(); i++)
        p(*i);
}

}  // namespace mongo
