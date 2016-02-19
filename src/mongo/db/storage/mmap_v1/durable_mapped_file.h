// durable_mapped_file.h

/*
*
*    Copyright (C) 2008 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/db/storage/mmap_v1/mmap.h"
#include "mongo/db/storage/paths.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

/**
 * DurableMappedFile adds some layers atop memory mapped files - specifically our handling of
 * private views & such. if you don't care about journaling/durability (temp sort files & such) use
 * MemoryMappedFile class, not this.
 */
class DurableMappedFile : private MemoryMappedFile {
protected:
    virtual void* viewForFlushing() {
        return _view_write;
    }

public:
    DurableMappedFile(OptionSet options = NONE);
    virtual ~DurableMappedFile();

    /** @return true if opened ok. */
    bool open(const std::string& fname);

    /** @return file length */
    unsigned long long length() const {
        return MemoryMappedFile::length();
    }

    std::string filename() const {
        return MemoryMappedFile::filename();
    }

    void flush(bool sync) {
        MemoryMappedFile::flush(sync);
    }

    /* Creates with length if DNE, otherwise uses existing file length,
       passed length.
       @return true for ok
    */
    bool create(const std::string& fname, unsigned long long& len);

    /* Get the "standard" view (which is the private one).
       @return the private view.
    */
    void* getView() const {
        return _view_private;
    }

    /* Get the "write" view (which is required for writing).
       @return the write view.
    */
    void* view_write() const {
        return _view_write;
    }

    /** for a filename a/b/c.3
        filePath() is "a/b/c"
        fileSuffixNo() is 3
        if the suffix is "ns", fileSuffixNo -1
    */
    const RelativePath& relativePath() const {
        DEV verify(!_p._p.empty());
        return _p;
    }

    int fileSuffixNo() const {
        return _fileSuffixNo;
    }
    HANDLE getFd() {
        return MemoryMappedFile::getFd();
    }

    /** true if we have written.
        set in PREPLOGBUFFER, it is NOT set immediately on write intent declaration.
        reset to false in REMAPPRIVATEVIEW
    */
    bool willNeedRemap() {
        return _willNeedRemap;
    }
    void setWillNeedRemap() {
        _willNeedRemap = true;
    }

    void remapThePrivateView();

    virtual bool isDurableMappedFile() {
        return true;
    }

private:
    void* _view_write;
    void* _view_private;
    bool _willNeedRemap;
    RelativePath _p;    // e.g. "somepath/dbname"
    int _fileSuffixNo;  // e.g. 3.  -1="ns"

    void setPath(const std::string& pathAndFileName);
    bool finishOpening();
};


#ifdef _WIN32
// Simple array based bitset to track COW chunks in memory mapped files on Windows
// A chunk is a 64MB granular region in virtual memory that we mark as COW everytime we need
// to write to a memory mapped files on Windows
//
class MemoryMappedCOWBitset {
    MONGO_DISALLOW_COPYING(MemoryMappedCOWBitset);

public:
    // Size of the chunks we mark Copy-On-Write with VirtualProtect
    static const unsigned long long ChunkSize = 64 * 1024 * 1024;

    // Number of chunks we store in our bitset which are really 32-bit ints
    static const unsigned long long NChunks = 64 * 1024;

    // Total Virtual Memory space we can cover with the bitset
    static const unsigned long long MaxChunkMemory = ChunkSize * NChunks * sizeof(unsigned int) * 8;

    // Size in bytes of the bitset we allocate
    static const unsigned long long MaxChunkBytes = NChunks * sizeof(unsigned int);

    // 128 TB Virtual Memory space in Windows 8.1/2012 R2, 8TB before
    static const unsigned long long MaxWinMemory = 128ULL * 1024 * 1024 * 1024 * 1024;

    // Make sure that the chunk memory covers the Max Windows user process VM space
    static_assert(MaxChunkMemory == MaxWinMemory,
                  "Need a larger bitset to cover max process VM space");

public:
    MemoryMappedCOWBitset() {
        static_assert(MemoryMappedCOWBitset::MaxChunkBytes == sizeof(bits),
                      "Validate our predicted bitset size is correct");
    }

    bool get(uintptr_t i) const {
        uintptr_t x = i / 32;
        verify(x < MemoryMappedCOWBitset::NChunks);
        return (bits[x].loadRelaxed() & (1 << (i % 32))) != 0;
    }

    // Note: assumes caller holds privateViews.mutex
    void set(uintptr_t i) {
        uintptr_t x = i / 32;
        verify(x < MemoryMappedCOWBitset::NChunks);
        bits[x].store(bits[x].loadRelaxed() | (1 << (i % 32)));
    }

    // Note: assumes caller holds privateViews.mutex
    void clear(uintptr_t i) {
        uintptr_t x = i / 32;
        verify(x < MemoryMappedCOWBitset::NChunks);
        bits[x].store(bits[x].loadRelaxed() & ~(1 << (i % 32)));
    }

private:
    // atomic as we are doing double check locking
    AtomicUInt32 bits[MemoryMappedCOWBitset::NChunks];
};
#endif

/** for durability support we want to be able to map pointers to specific DurableMappedFile objects.
*/
class PointerToDurableMappedFile {
    MONGO_DISALLOW_COPYING(PointerToDurableMappedFile);

public:
    PointerToDurableMappedFile();

    /** register view.
        not-threadsafe, caller must hold _mutex()
    */
    void add_inlock(void* view, DurableMappedFile* f);

    /** de-register view.
        threadsafe
        */
    void remove(void* view, size_t length);

    /** find associated MMF object for a given pointer.
        threadsafe
        @param ofs out returns offset into the view of the pointer, if found.
        @return the DurableMappedFile to which this pointer belongs. null if not found.
    */
    DurableMappedFile* find(void* p, /*out*/ size_t& ofs);

    /** for doing many finds in a row with one lock operation */
    stdx::mutex& _mutex() {
        return _m;
    }

    /** not-threadsafe, caller must hold _mutex() */
    DurableMappedFile* find_inlock(void* p, /*out*/ size_t& ofs);

    /** not-threadsafe, caller must hold _mutex() */
    unsigned numberOfViews_inlock() const {
        return _views.size();
    }

    /** make the private map range writable (necessary for our windows implementation) */
    void makeWritable(void*, unsigned len);

    void clearWritableBits(void* privateView, size_t len);

private:
    void clearWritableBits_inlock(void* privateView, size_t len);

#ifdef _WIN32
    void makeChunkWritable(size_t chunkno);
#endif

private:
    // PointerToDurableMappedFile Mutex
    //
    // Protects:
    //  Protects internal consistency of data structure
    // Lock Ordering:
    //  Must be taken before MapViewMutex if both are taken to prevent deadlocks
    stdx::mutex _m;
    std::map<void*, DurableMappedFile*> _views;

#ifdef _WIN32
    // Tracks which memory mapped regions are marked as Copy on Write
    MemoryMappedCOWBitset writable;
#endif
};

#ifdef _WIN32
inline void PointerToDurableMappedFile::makeWritable(void* privateView, unsigned len) {
    size_t p = reinterpret_cast<size_t>(privateView);
    unsigned a = p / MemoryMappedCOWBitset::ChunkSize;
    unsigned b = (p + len) / MemoryMappedCOWBitset::ChunkSize;

    for (unsigned i = a; i <= b; i++) {
        if (!writable.get(i)) {
            makeChunkWritable(i);
        }
    }
}
#else
inline void PointerToDurableMappedFile::makeWritable(void* _p, unsigned len) {}
#endif

// allows a pointer into any private view of a DurableMappedFile to be resolved to the
// DurableMappedFile object
extern PointerToDurableMappedFile privateViews;
}
