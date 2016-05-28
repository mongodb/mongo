// @file file_allocator.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/file_allocator.h"

#include <boost/filesystem/operations.hpp>
#include <errno.h>
#include <fcntl.h>

#if defined(__FreeBSD__)
#include <sys/mount.h>
#include <sys/param.h>
#endif

#if defined(__linux__)
#include <sys/vfs.h>
#endif

#if defined(_WIN32)
#include <io.h>
#endif

#include "mongo/db/storage/paths.h"
#include "mongo/platform/posix_fadvise.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

using namespace mongoutils;

#ifndef O_NOATIME
#define O_NOATIME (0)
#endif

namespace mongo {

using std::endl;
using std::list;
using std::string;
using std::stringstream;

// unique number for temporary file names
unsigned long long FileAllocator::_uniqueNumber = 0;
static SimpleMutex _uniqueNumberMutex;

MONGO_FP_DECLARE(allocateDiskFull);

/**
 * Aliases for Win32 CRT functions
 */
#if defined(_WIN32)
static inline long lseek(int fd, long offset, int origin) {
    return _lseek(fd, offset, origin);
}
static inline int write(int fd, const void* data, int count) {
    return _write(fd, data, count);
}
static inline int close(int fd) {
    return _close(fd);
}

typedef BOOL(CALLBACK* GetVolumeInformationByHandleWPtr)(
    HANDLE, LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPWSTR, DWORD);
GetVolumeInformationByHandleWPtr GetVolumeInformationByHandleWFunc;

MONGO_INITIALIZER(InitGetVolumeInformationByHandleW)(InitializerContext* context) {
    HMODULE kernelLib = LoadLibraryA("kernel32.dll");
    if (kernelLib) {
        GetVolumeInformationByHandleWFunc = reinterpret_cast<GetVolumeInformationByHandleWPtr>(
            GetProcAddress(kernelLib, "GetVolumeInformationByHandleW"));
    }
    return Status::OK();
}
#endif

boost::filesystem::path ensureParentDirCreated(const boost::filesystem::path& p) {
    const boost::filesystem::path parent = p.branch_path();

    if (!boost::filesystem::exists(parent)) {
        ensureParentDirCreated(parent);
        log() << "creating directory " << parent.string() << endl;
        boost::filesystem::create_directory(parent);
        flushMyDirectory(parent);  // flushes grandparent to ensure parent exists after crash
    }

    verify(boost::filesystem::is_directory(parent));
    return parent;
}

FileAllocator::FileAllocator() : _failed() {}


void FileAllocator::start() {
    stdx::thread t(stdx::bind(&FileAllocator::run, this));
    t.detach();
}

void FileAllocator::requestAllocation(const string& name, long& size) {
    stdx::lock_guard<stdx::mutex> lk(_pendingMutex);
    if (_failed)
        return;
    long oldSize = prevSize(name);
    if (oldSize != -1) {
        size = oldSize;
        return;
    }
    _pending.push_back(name);
    _pendingSize[name] = size;
    _pendingUpdated.notify_all();
}

void FileAllocator::allocateAsap(const string& name, unsigned long long& size) {
    stdx::unique_lock<stdx::mutex> lk(_pendingMutex);

    // In case the allocator is in failed state, check once before starting so that subsequent
    // requests for the same database would fail fast after the first one has failed.
    checkFailure();

    long oldSize = prevSize(name);
    if (oldSize != -1) {
        size = oldSize;
        if (!inProgress(name))
            return;
    }
    checkFailure();
    _pendingSize[name] = size;
    if (_pending.size() == 0)
        _pending.push_back(name);
    else if (_pending.front() != name) {
        _pending.remove(name);
        list<string>::iterator i = _pending.begin();
        ++i;
        _pending.insert(i, name);
    }
    _pendingUpdated.notify_all();
    while (inProgress(name)) {
        checkFailure();
        _pendingUpdated.wait(lk);
    }
}

void FileAllocator::waitUntilFinished() const {
    if (_failed)
        return;
    stdx::unique_lock<stdx::mutex> lk(_pendingMutex);
    while (_pending.size() != 0)
        _pendingUpdated.wait(lk);
}

// TODO: pull this out to per-OS files once they exist
static bool useSparseFiles(int fd) {
#if defined(__linux__) || defined(__FreeBSD__)
    struct statfs fs_stats;
    int ret = fstatfs(fd, &fs_stats);
    uassert(16062, "fstatfs failed: " + errnoWithDescription(), ret == 0);
#endif

#if defined(__linux__)
// these are from <linux/magic.h> but that isn't available on all systems
#define NFS_SUPER_MAGIC 0x6969
#define TMPFS_MAGIC 0x01021994
#define ZFS_SUPER_MAGIC 0x2fc12fc1
    return (fs_stats.f_type == NFS_SUPER_MAGIC) || (fs_stats.f_type == TMPFS_MAGIC) ||
        (fs_stats.f_type == ZFS_SUPER_MAGIC);

#elif defined(__FreeBSD__)

    return (str::equals(fs_stats.f_fstypename, "zfs") ||
            str::equals(fs_stats.f_fstypename, "nfs") ||
            str::equals(fs_stats.f_fstypename, "oldnfs"));

#elif defined(__sun)
    // assume using ZFS which is copy-on-write so no benefit to zero-filling
    // TODO: check which fs we are using like we do elsewhere
    return true;
#else
    return false;
#endif
}

#if defined(_WIN32)
static bool isFileOnNTFSVolume(int fd) {
    if (!GetVolumeInformationByHandleWFunc) {
        warning() << "Could not retrieve pointer to GetVolumeInformationByHandleW function";
        return false;
    }

    HANDLE fileHandle = (HANDLE)_get_osfhandle(fd);
    if (fileHandle == INVALID_HANDLE_VALUE) {
        warning() << "_get_osfhandle() failed with " << _strerror(NULL);
        return false;
    }

    WCHAR fileSystemName[MAX_PATH + 1];
    if (!GetVolumeInformationByHandleWFunc(
            fileHandle, NULL, 0, NULL, 0, NULL, fileSystemName, sizeof(fileSystemName))) {
        DWORD gle = GetLastError();
        warning() << "GetVolumeInformationByHandleW failed with " << errnoWithDescription(gle);
        return false;
    }

    return lstrcmpW(fileSystemName, L"NTFS") == 0;
}
#endif

void FileAllocator::ensureLength(int fd, long size) {
    // Test running out of disk scenarios
    if (MONGO_FAIL_POINT(allocateDiskFull)) {
        uasserted(10444, "File allocation failed due to failpoint.");
    }

#if !defined(_WIN32)
    if (useSparseFiles(fd)) {
        LOG(1) << "using ftruncate to create a sparse file" << endl;
        int ret = ftruncate(fd, size);
        uassert(16063, "ftruncate failed: " + errnoWithDescription(), ret == 0);
        return;
    }
#endif

#if defined(__linux__)
    int ret = posix_fallocate(fd, 0, size);
    if (ret == 0)
        return;

    log() << "FileAllocator: posix_fallocate failed: " << errnoWithDescription(ret)
          << " falling back" << endl;
#endif

    off_t filelen = lseek(fd, 0, SEEK_END);
    if (filelen < size) {
        if (filelen != 0) {
            stringstream ss;
            ss << "failure creating new datafile; lseek failed for fd " << fd
               << " with errno: " << errnoWithDescription();
            uassert(10440, ss.str(), filelen == 0);
        }
        // Check for end of disk.

        uassert(10441,
                str::stream() << "Unable to allocate new file of size " << size << ' '
                              << errnoWithDescription(),
                size - 1 == lseek(fd, size - 1, SEEK_SET));
        uassert(10442,
                str::stream() << "Unable to allocate new file of size " << size << ' '
                              << errnoWithDescription(),
                1 == write(fd, "", 1));

        // File expansion is completed here. Do not do the zeroing out on OS-es where there
        // is no risk of triggering allocation-related bugs such as
        // http://support.microsoft.com/kb/2731284.
        //
        if (!ProcessInfo::isDataFileZeroingNeeded()) {
            return;
        }

#if defined(_WIN32)
        if (!isFileOnNTFSVolume(fd)) {
            log() << "No need to zero out datafile on non-NTFS volume" << endl;
            return;
        }
#endif

        lseek(fd, 0, SEEK_SET);

        const long z = 256 * 1024;
        const std::unique_ptr<char[]> buf_holder(new char[z]);
        char* buf = buf_holder.get();
        memset(buf, 0, z);
        long left = size;
        while (left > 0) {
            long towrite = left;
            if (towrite > z)
                towrite = z;

            int written = write(fd, buf, towrite);
            uassert(10443, errnoWithPrefix("FileAllocator: file write failed"), written > 0);
            left -= written;
        }
    }
}

void FileAllocator::checkFailure() {
    if (_failed) {
        // we want to log the problem (diskfull.js expects it) but we do not want to dump a stack
        // trace
        msgassertedNoTrace(12520, "new file allocation failure");
    }
}

long FileAllocator::prevSize(const string& name) const {
    if (_pendingSize.count(name) > 0)
        return _pendingSize[name];
    if (boost::filesystem::exists(name))
        return boost::filesystem::file_size(name);
    return -1;
}

// caller must hold _pendingMutex lock.
bool FileAllocator::inProgress(const string& name) const {
    for (list<string>::const_iterator i = _pending.begin(); i != _pending.end(); ++i)
        if (*i == name)
            return true;
    return false;
}

string FileAllocator::makeTempFileName(boost::filesystem::path root) {
    while (1) {
        boost::filesystem::path p = root / "_tmp";
        stringstream ss;
        unsigned long long thisUniqueNumber;
        {
            // increment temporary file name counter
            // TODO: SERVER-6055 -- Unify temporary file name selection
            stdx::lock_guard<SimpleMutex> lk(_uniqueNumberMutex);
            thisUniqueNumber = _uniqueNumber;
            ++_uniqueNumber;
        }
        ss << thisUniqueNumber;
        p /= ss.str();
        string fn = p.string();
        if (!boost::filesystem::exists(p))
            return fn;
    }
    return "";
}

void FileAllocator::run(FileAllocator* fa) {
    setThreadName("FileAllocator");
    {
        // initialize unique temporary file name counter
        // TODO: SERVER-6055 -- Unify temporary file name selection
        stdx::lock_guard<SimpleMutex> lk(_uniqueNumberMutex);
        _uniqueNumber = curTimeMicros64();
    }
    while (1) {
        {
            stdx::unique_lock<stdx::mutex> lk(fa->_pendingMutex);
            if (fa->_pending.size() == 0)
                fa->_pendingUpdated.wait(lk);
        }
        while (1) {
            string name;
            long size = 0;
            {
                stdx::lock_guard<stdx::mutex> lk(fa->_pendingMutex);
                if (fa->_pending.size() == 0)
                    break;
                name = fa->_pending.front();
                size = fa->_pendingSize[name];
            }

            string tmp;
            long fd = 0;
            try {
                log() << "allocating new datafile " << name << ", filling with zeroes..." << endl;

                boost::filesystem::path parent = ensureParentDirCreated(name);
                tmp = fa->makeTempFileName(parent);
                ensureParentDirCreated(tmp);

#if defined(_WIN32)
                fd = _open(tmp.c_str(), _O_RDWR | _O_CREAT | O_NOATIME, _S_IREAD | _S_IWRITE);
#else
                fd = open(tmp.c_str(), O_CREAT | O_RDWR | O_NOATIME, S_IRUSR | S_IWUSR);
#endif
                if (fd < 0) {
                    log() << "FileAllocator: couldn't create " << name << " (" << tmp << ") "
                          << errnoWithDescription() << endl;
                    uasserted(10439, "");
                }

#if defined(POSIX_FADV_DONTNEED)
                if (posix_fadvise(fd, 0, size, POSIX_FADV_DONTNEED)) {
                    log() << "warning: posix_fadvise fails " << name << " (" << tmp << ") "
                          << errnoWithDescription() << endl;
                }
#endif

                Timer t;

                /* make sure the file is the full desired length */
                ensureLength(fd, size);

                close(fd);
                fd = 0;

                if (rename(tmp.c_str(), name.c_str())) {
                    const string& errStr = errnoWithDescription();
                    const string& errMessage = str::stream() << "error: couldn't rename " << tmp
                                                             << " to " << name << ' ' << errStr;
                    msgasserted(13653, errMessage);
                }
                flushMyDirectory(name);

                log() << "done allocating datafile " << name << ", "
                      << "size: " << size / 1024 / 1024 << "MB, "
                      << " took " << ((double)t.millis()) / 1000.0 << " secs" << endl;

                // no longer in a failed state. allow new writers.
                fa->_failed = false;
            } catch (const std::exception& e) {
                log() << "error: failed to allocate new file: " << name << " size: " << size << ' '
                      << e.what() << ".  will try again in 10 seconds" << endl;
                if (fd > 0)
                    close(fd);
                try {
                    if (!tmp.empty())
                        boost::filesystem::remove(tmp);
                    boost::filesystem::remove(name);
                } catch (const std::exception& e) {
                    log() << "error removing files: " << e.what() << endl;
                }

                {
                    stdx::lock_guard<stdx::mutex> lk(fa->_pendingMutex);
                    fa->_failed = true;

                    // TODO: Should we remove the file from pending?
                    fa->_pendingUpdated.notify_all();
                }


                sleepsecs(10);
                continue;
            }

            {
                stdx::lock_guard<stdx::mutex> lk(fa->_pendingMutex);
                fa->_pendingSize.erase(name);
                fa->_pending.pop_front();
                fa->_pendingUpdated.notify_all();
            }
        }
    }
}

FileAllocator* FileAllocator::_instance = 0;

FileAllocator* FileAllocator::get() {
    if (!_instance)
        _instance = new FileAllocator();
    return _instance;
}

}  // namespace mongo
