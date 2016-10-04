// mmap_win.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/mmap.h"

#include "mongo/db/storage/mmap_v1/durable_mapped_file.h"
#include "mongo/db/storage/mmap_v1/file_allocator.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/text.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::endl;
using std::string;
using std::vector;

static size_t fetchMinOSPageSizeBytes() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t minOSPageSizeBytes = si.dwPageSize;
    minOSPageSizeBytesTest(minOSPageSizeBytes);
    return minOSPageSizeBytes;
}
const size_t g_minOSPageSizeBytes = fetchMinOSPageSizeBytes();

// MapViewMutex
//
// Protects:
//   1. Ensures all MapViewOfFile/UnMapViewOfFile operations are serialized to reduce chance of
//    "address in use" errors (error code 487)
// -   These errors can still occur if the memory is used for other purposes
//     (stack storage, heap)
//   2. Prevents calls to VirtualProtect while we remapping files.
// Lock Ordering:
//  - If taken, must be after previewViews._m to prevent deadlocks
stdx::mutex mapViewMutex;

MAdvise::MAdvise(void*, unsigned, Advice) {}
MAdvise::~MAdvise() {}

const unsigned long long memoryMappedFileLocationFloor = 256LL * 1024LL * 1024LL * 1024LL;
static unsigned long long _nextMemoryMappedFileLocation = memoryMappedFileLocationFloor;

// nextMemoryMappedFileLocationMutex
//
// Protects:
//  Windows 64-bit specific allocation of virtual memory regions for
//  placing memory mapped files in memory
// Lock Ordering:
//  No restrictions
static SimpleMutex _nextMemoryMappedFileLocationMutex;

unsigned long long AlignNumber(unsigned long long number, unsigned long long granularity) {
    return (number + granularity - 1) & ~(granularity - 1);
}

static void* getNextMemoryMappedFileLocation(unsigned long long mmfSize) {
    if (4 == sizeof(void*)) {
        return 0;
    }
    stdx::lock_guard<SimpleMutex> lk(_nextMemoryMappedFileLocationMutex);

    static unsigned long long granularity = 0;

    if (0 == granularity) {
        SYSTEM_INFO systemInfo;
        GetSystemInfo(&systemInfo);
        granularity = static_cast<unsigned long long>(systemInfo.dwAllocationGranularity);
    }

    unsigned long long thisMemoryMappedFileLocation = _nextMemoryMappedFileLocation;

    int current_retry = 1;

    while (true) {
        MEMORY_BASIC_INFORMATION memInfo;

        if (VirtualQuery(reinterpret_cast<LPCVOID>(thisMemoryMappedFileLocation),
                         &memInfo,
                         sizeof(memInfo)) == 0) {
            DWORD gle = GetLastError();

            // If we exceed the limits of Virtual Memory
            // - 8TB before Windows 8.1/2012 R2, 128 TB after
            // restart scanning from our memory mapped floor once more
            // This is a linear scan of regions, not of every VM page
            if (gle == ERROR_INVALID_PARAMETER && current_retry == 1) {
                thisMemoryMappedFileLocation = memoryMappedFileLocationFloor;
                ++current_retry;
                continue;
            }

            log() << "VirtualQuery of " << thisMemoryMappedFileLocation << " failed with error "
                  << errnoWithDescription(gle);
            fassertFailed(17484);
        }

        // Free memory regions that we can use for memory map files
        // 1. Marked MEM_FREE, not MEM_RESERVE
        // 2. Marked as PAGE_NOACCESS, not anything else
        if (memInfo.Protect == PAGE_NOACCESS && memInfo.State == MEM_FREE &&
            memInfo.RegionSize > mmfSize)
            break;

        // Align the memory location in case RegionSize is not aligned to the OS allocation
        // granularity size
        thisMemoryMappedFileLocation = AlignNumber(
            reinterpret_cast<unsigned long long>(memInfo.BaseAddress) + memInfo.RegionSize,
            granularity);
    }

    _nextMemoryMappedFileLocation =
        thisMemoryMappedFileLocation + AlignNumber(mmfSize, granularity);

    return reinterpret_cast<void*>(static_cast<uintptr_t>(thisMemoryMappedFileLocation));
}

void MemoryMappedFile::close() {
    LockMongoFilesShared::assertExclusivelyLocked();

    // Prevent flush and close from concurrently running
    stdx::lock_guard<stdx::mutex> lk(_flushMutex);

    {
        stdx::lock_guard<stdx::mutex> lk(mapViewMutex);

        for (vector<void*>::iterator i = views.begin(); i != views.end(); i++) {
            UnmapViewOfFile(*i);
        }
    }

    views.clear();
    if (maphandle)
        CloseHandle(maphandle);
    maphandle = 0;
    if (fd)
        CloseHandle(fd);
    fd = 0;
    destroyed();  // cleans up from the master list of mmaps
}

unsigned long long mapped = 0;

void* MemoryMappedFile::map(const char* filenameIn, unsigned long long& length) {
    verify(fd == 0 && len == 0);  // can't open more than once
    setFilename(filenameIn);
    FileAllocator::get()->allocateAsap(filenameIn, length);
    /* big hack here: Babble uses db names with colons.  doesn't seem to work on windows.  temporary
     * perhaps. */
    char filename[256];
    strncpy(filename, filenameIn, 255);
    filename[255] = 0;
    {
        size_t len = strlen(filename);
        for (size_t i = len - 1; i >= 0; i--) {
            if (filename[i] == '/' || filename[i] == '\\')
                break;

            if (filename[i] == ':')
                filename[i] = '_';
        }
    }

    updateLength(filename, length);

    const bool readOnly = isOptionSet(READONLY);

    {
        DWORD createOptions = FILE_ATTRIBUTE_NORMAL;
        if (isOptionSet(SEQUENTIAL))
            createOptions |= FILE_FLAG_SEQUENTIAL_SCAN;

        DWORD desiredAccess = readOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
        DWORD shareMode = readOnly ? FILE_SHARE_READ : (FILE_SHARE_WRITE | FILE_SHARE_READ);

        fd = CreateFileW(toWideString(filename).c_str(),
                         desiredAccess,  // desired access
                         shareMode,      // share mode
                         NULL,           // security
                         OPEN_ALWAYS,    // create disposition
                         createOptions,  // flags
                         NULL);          // hTempl
        if (fd == INVALID_HANDLE_VALUE) {
            DWORD dosError = GetLastError();
            log() << "CreateFileW for " << filename << " failed with "
                  << errnoWithDescription(dosError) << " (file size is " << length << ")"
                  << " in MemoryMappedFile::map" << endl;
            return 0;
        }
    }

    mapped += length;

    {
        DWORD flProtect = readOnly ? PAGE_READONLY : PAGE_READWRITE;
        maphandle = CreateFileMappingW(fd,
                                       NULL,
                                       flProtect,
                                       length >> 32 /*maxsizehigh*/,
                                       (unsigned)length /*maxsizelow*/,
                                       NULL /*lpName*/);
        if (maphandle == NULL) {
            DWORD dosError = GetLastError();
            log() << "CreateFileMappingW for " << filename << " failed with "
                  << errnoWithDescription(dosError) << " (file size is " << length << ")"
                  << " in MemoryMappedFile::map" << endl;
            close();
            fassertFailed(16225);
        }
    }

    void* view = 0;
    {
        stdx::lock_guard<stdx::mutex> lk(mapViewMutex);
        DWORD access = readOnly ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;

        int current_retry = 0;
        while (true) {
            LPVOID thisAddress = getNextMemoryMappedFileLocation(length);

            view = MapViewOfFileEx(maphandle,  // file mapping handle
                                   access,     // access
                                   0,
                                   0,             // file offset, high and low
                                   0,             // bytes to map, 0 == all
                                   thisAddress);  // address to place file

            if (view == 0) {
                DWORD dosError = GetLastError();

                ++current_retry;

                // If we failed to allocate a memory mapped file, try again in case we picked
                // an address that Windows is also trying to use for some other VM allocations
                if (dosError == ERROR_INVALID_ADDRESS && current_retry < 5) {
                    continue;
                }

#ifndef _WIN64
                // Warn user that if they are running a 32-bit app on 64-bit Windows
                if (dosError == ERROR_NOT_ENOUGH_MEMORY) {
                    BOOL wow64Process;
                    BOOL retWow64 = IsWow64Process(GetCurrentProcess(), &wow64Process);
                    if (retWow64 && wow64Process) {
                        log() << "This is a 32-bit MongoDB binary running on a 64-bit"
                                 " operating system that has run out of virtual memory for"
                                 " databases. Switch to a 64-bit build of MongoDB to open"
                                 " the databases.";
                    }
                }
#endif

                log() << "MapViewOfFileEx for " << filename << " at address " << thisAddress
                      << " failed with " << errnoWithDescription(dosError) << " (file size is "
                      << length << ")"
                      << " in MemoryMappedFile::map" << endl;

                close();
                fassertFailed(16166);
            }

            break;
        }
    }

    views.push_back(view);
    len = length;
    return view;
}

extern stdx::mutex mapViewMutex;

void* MemoryMappedFile::createPrivateMap() {
    verify(maphandle);

    stdx::lock_guard<stdx::mutex> lk(mapViewMutex);

    LPVOID thisAddress = getNextMemoryMappedFileLocation(len);

    void* privateMapAddress = NULL;
    int current_retry = 0;

    while (true) {
        privateMapAddress = MapViewOfFileEx(maphandle,      // file mapping handle
                                            FILE_MAP_READ,  // access
                                            0,
                                            0,             // file offset, high and low
                                            0,             // bytes to map, 0 == all
                                            thisAddress);  // address to place file

        if (privateMapAddress == 0) {
            DWORD dosError = GetLastError();

            ++current_retry;

            // If we failed to allocate a memory mapped file, try again in case we picked
            // an address that Windows is also trying to use for some other VM allocations
            if (dosError == ERROR_INVALID_ADDRESS && current_retry < 5) {
                continue;
            }

            log() << "MapViewOfFileEx for " << filename() << " failed with error "
                  << errnoWithDescription(dosError) << " (file size is " << len << ")"
                  << " in MemoryMappedFile::createPrivateMap" << endl;

            fassertFailed(16167);
        }

        break;
    }

    views.push_back(privateMapAddress);
    return privateMapAddress;
}

void* MemoryMappedFile::remapPrivateView(void* oldPrivateAddr) {
    LockMongoFilesExclusive lockMongoFiles;

    privateViews.clearWritableBits(oldPrivateAddr, len);

    stdx::lock_guard<stdx::mutex> lk(mapViewMutex);

    if (!UnmapViewOfFile(oldPrivateAddr)) {
        DWORD dosError = GetLastError();
        log() << "UnMapViewOfFile for " << filename() << " failed with error "
              << errnoWithDescription(dosError) << " in MemoryMappedFile::remapPrivateView" << endl;
        fassertFailed(16168);
    }

    void* newPrivateView =
        MapViewOfFileEx(maphandle,      // file mapping handle
                        FILE_MAP_READ,  // access
                        0,
                        0,                // file offset, high and low
                        0,                // bytes to map, 0 == all
                        oldPrivateAddr);  // we want the same address we had before
    if (0 == newPrivateView) {
        DWORD dosError = GetLastError();
        log() << "MapViewOfFileEx for " << filename() << " failed with error "
              << errnoWithDescription(dosError) << " (file size is " << len << ")"
              << " in MemoryMappedFile::remapPrivateView" << endl;
    }
    fassert(16148, newPrivateView == oldPrivateAddr);
    return newPrivateView;
}

class WindowsFlushable : public MemoryMappedFile::Flushable {
public:
    WindowsFlushable(MemoryMappedFile* theFile,
                     void* view,
                     HANDLE fd,
                     const uint64_t id,
                     const std::string& filename,
                     stdx::mutex& flushMutex)
        : _theFile(theFile),
          _view(view),
          _fd(fd),
          _id(id),
          _filename(filename),
          _flushMutex(flushMutex) {}

    void flush() {
        if (!_view || !_fd)
            return;

        {
            LockMongoFilesShared mmfilesLock;

            std::set<MongoFile*> mmfs = MongoFile::getAllFiles();
            std::set<MongoFile*>::const_iterator it = mmfs.find(_theFile);
            if (it == mmfs.end() || (*it)->getUniqueId() != _id) {
                // this was deleted while we were unlocked
                return;
            }

            // Hold the flush mutex to ensure the file is not closed during flush
            _flushMutex.lock();
        }

        stdx::lock_guard<stdx::mutex> lk(_flushMutex, stdx::adopt_lock);

        int loopCount = 0;
        bool success = false;
        bool timeout = false;
        int dosError = ERROR_SUCCESS;
        const int maximumTimeInSeconds = 60 * 15;
        Timer t;
        while (!success && !timeout) {
            ++loopCount;
            success = FALSE != FlushViewOfFile(_view, 0);
            if (!success) {
                dosError = GetLastError();
                if (dosError != ERROR_LOCK_VIOLATION) {
                    break;
                }
                timeout = t.seconds() > maximumTimeInSeconds;
            }
        }
        if (success && loopCount > 1) {
            log() << "FlushViewOfFile for " << _filename << " succeeded after " << loopCount
                  << " attempts taking " << t.millis() << "ms" << endl;
        } else if (!success) {
            log() << "FlushViewOfFile for " << _filename << " failed with error " << dosError
                  << " after " << loopCount << " attempts taking " << t.millis() << "ms" << endl;
            // Abort here to avoid data corruption
            fassert(16387, false);
        }

        success = FALSE != FlushFileBuffers(_fd);
        if (!success) {
            int err = GetLastError();
            log() << "FlushFileBuffers failed: " << errnoWithDescription(err)
                  << " file: " << _filename << endl;
            dataSyncFailedHandler();
        }
    }

    MemoryMappedFile* _theFile;  // this may be deleted while we are running
    void* _view;
    HANDLE _fd;
    const uint64_t _id;
    string _filename;
    stdx::mutex& _flushMutex;
};

void MemoryMappedFile::flush(bool sync) {
    invariant(!(isOptionSet(Options::READONLY)));
    uassert(13056, "Async flushing not supported on windows", sync);
    if (!views.empty()) {
        WindowsFlushable f(this, viewForFlushing(), fd, _uniqueId, filename(), _flushMutex);
        f.flush();
    }
}

MemoryMappedFile::Flushable* MemoryMappedFile::prepareFlush() {
    return new WindowsFlushable(this, viewForFlushing(), fd, _uniqueId, filename(), _flushMutex);
}
}
