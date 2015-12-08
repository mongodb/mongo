/*    Copyright 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/base/secure_allocator.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/secure_zero_memory.h"

namespace mongo {

/**
 * NOTE(jcarey): Why not new/delete?
 *
 * As a general rule, mlock/virtuallock lack any kind of recursive semantics
 * (they free any locks on the underlying page if called once). While some
 * platforms do offer those semantics, they're not available globally, so we
 * have to flow all allocations through page based allocations.
 */
namespace secure_allocator_details {

#ifdef _WIN32

void* allocate(std::size_t bytes) {
    // Flags:
    //
    // MEM_COMMIT - allocates the memory charges and zeros the underlying
    //              memory
    // MEM_RESERVE - Reserves space in the process's virtual address space
    //
    // The two flags together give us bytes that are attached to the process
    // that we can actually write to.
    //
    // PAGE_READWRITE - allows read/write access to the page
    auto ptr = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!ptr) {
        auto str = errnoWithPrefix("Failed to VirtualAlloc");
        severe() << str;
        fassertFailed(28835);
    }

    if (VirtualLock(ptr, bytes) == 0) {
        auto str = errnoWithPrefix("Failed to VirtualLock");
        severe() << str;
        fassertFailed(28828);
    }

    return ptr;
}

void deallocate(void* ptr, std::size_t bytes) {
    secureZeroMemory(ptr, bytes);

    if (VirtualUnlock(ptr, bytes) == 0) {
        auto str = errnoWithPrefix("Failed to VirtualUnlock");
        severe() << str;
        fassertFailed(28829);
    }

    // VirtualFree needs to take 0 as the size parameter for MEM_RELEASE
    // (that's how the api works).
    if (VirtualFree(ptr, 0, MEM_RELEASE) == 0) {
        auto str = errnoWithPrefix("Failed to VirtualFree");
        severe() << str;
        fassertFailed(28830);
    }
}

#else

// See https://github.com/libressl-portable/portable/issues/24 for the table
// that suggests this approach. This assumes that MAP_ANONYMOUS and MAP_ANON are
// macro definitions, but that seems plausible on all platforms we care about.

#if defined(MAP_ANONYMOUS)
#define MONGO_MAP_ANONYMOUS MAP_ANONYMOUS
#else
#if defined(MAP_ANON)
#define MONGO_MAP_ANONYMOUS MAP_ANON
#endif
#endif

#if !defined(MONGO_MAP_ANONYMOUS)
#error "Could not determine a way to map anonymous memory, required for secure allocation"
#endif

void* allocate(std::size_t bytes) {
    // Flags:
    //
    // PROT_READ | PROT_WRITE - allows read write access to the page
    //
    // MAP_PRIVATE - Ensure that the mapping is copy-on-write. Otherwise writes
    //               made in this process can be seen in children.
    //
    // MAP_ANONYMOUS - The mapping is not backed by a file. fd must be -1 on
    //                 some platforms, offset is ignored (so 0).
    //
    // skipping flags like MAP_LOCKED and MAP_POPULATE as linux-isms
    auto ptr =
        mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MONGO_MAP_ANONYMOUS, -1, 0);

    if (!ptr) {
        auto str = errnoWithPrefix("Failed to mmap");
        severe() << str;
        fassertFailed(28831);
    }

    if (mlock(ptr, bytes) != 0) {
        auto str = errnoWithPrefix("Failed to mlock");
        severe() << str;
        fassertFailed(28832);
    }

    return ptr;
}

void deallocate(void* ptr, std::size_t bytes) {
    secureZeroMemory(ptr, bytes);

    if (munlock(ptr, bytes) != 0) {
        severe() << errnoWithPrefix("Failed to munlock");
        fassertFailed(28833);
    }

    if (munmap(ptr, bytes) != 0) {
        severe() << errnoWithPrefix("Failed to munmap");
        fassertFailed(28834);
    }
}

#endif

}  // namespace secure_allocator_details
}  // namespace mongo
