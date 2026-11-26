// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/logv2/log.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/text_huge_pages_params_gen.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <elf.h>
#include <link.h>

#include <sys/mman.h>
#include <sys/prctl.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25  // Linux 6.1+
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {

constexpr uintptr_t kHugePageSize = 2 * 1024 * 1024;

struct CodeRange {
    uintptr_t begin;
    uintptr_t end;
};

/**
 * Collects the 2MB-aligned parts of the main executable's code segments that were laid out for
 * huge pages (segment alignment of at least 2MB, e.g. the segment holding the BOLT-optimized code).
 */
int appendMainExecutableCodeRanges(dl_phdr_info* info, size_t size, void* data) {
    auto ranges = static_cast<std::vector<CodeRange>*>(data);
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || !(phdr.p_flags & PF_R) || !(phdr.p_flags & PF_X) ||
            (phdr.p_flags & PF_W) || phdr.p_align < kHugePageSize) {
            continue;
        }
        uintptr_t start = info->dlpi_addr + phdr.p_vaddr;
        uintptr_t begin = (start + kHugePageSize - 1) & ~(kHugePageSize - 1);
        uintptr_t end = (start + phdr.p_memsz) & ~(kHugePageSize - 1);
        if (begin < end) {
            ranges->push_back({begin, end});
        }
    }
    // The first object reported is the main executable; shared libraries are left alone.
    return 1;
}

bool transparentHugePagesDisabled() {
    if (prctl(PR_GET_THP_DISABLE, 0, 0, 0, 0) != 0) {
        return true;
    }
    std::ifstream f("/sys/kernel/mm/transparent_hugepage/enabled");
    std::string line;
    return !std::getline(f, line) || line.find("[never]") != std::string::npos;
}

/**
 * Replaces the file-backed 4KB-page mapping of 'range' by an anonymous copy backed by 2MB pages.
 * The copy is made read-only and executable before it is moved over the original with a single
 * mremap(), so the bytes at every address stay the same and threads executing code in the range
 * are not affected. Returns 0 on success or an errno value; on failure the original mapping is
 * kept (the kernel checks the map count limit before it unmaps the destination).
 */
int remapToHugePages(const CodeRange& range) {
    const size_t size = range.end - range.begin;
    void* reservation = mmap(nullptr,
                             size + kHugePageSize,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                             -1,
                             0);
    if (reservation == MAP_FAILED) {
        return errno;
    }
    // Trim the reservation to a 2MB-aligned range so that it can be backed by huge pages.
    auto reservationBegin = reinterpret_cast<uintptr_t>(reservation);
    auto reservationEnd = reservationBegin + size + kHugePageSize;
    auto copyBegin = (reservationBegin + kHugePageSize - 1) & ~(kHugePageSize - 1);
    if (copyBegin != reservationBegin) {
        munmap(reservation, copyBegin - reservationBegin);
    }
    if (copyBegin + size != reservationEnd) {
        munmap(reinterpret_cast<void*>(copyBegin + size), reservationEnd - (copyBegin + size));
    }
    auto copy = reinterpret_cast<void*>(copyBegin);
    auto discardCopy = [&](int error) {
        munmap(copy, size);
        return error;
    };

    if (madvise(copy, size, MADV_HUGEPAGE) != 0) {
        return discardCopy(errno);
    }
    std::memcpy(copy, reinterpret_cast<const void*>(range.begin), size);
    // The page faults of the copy fall back to 4KB pages if no huge page was available. Ask the
    // kernel to collapse them. Errors are ignored (e.g. kernels without MADV_COLLAPSE).
    (void)madvise(copy, size, MADV_COLLAPSE);
    if (mprotect(copy, size, PROT_READ | PROT_EXEC) != 0) {
        return discardCopy(errno);
    }
    if (mremap(copy,
               size,
               size,
               MREMAP_MAYMOVE | MREMAP_FIXED,
               reinterpret_cast<void*>(range.begin)) == MAP_FAILED) {
        return discardCopy(errno);
    }
    return 0;
}

// Runs before PinCodeSegments, so that mlock() applies to the new mapping.
MONGO_INITIALIZER_GENERAL(RemapCodeToHugePages, ("default"), ("PinCodeSegments"))
(InitializerContext*) {
    if (!gRemapCodeToHugePages || transparentHugePagesDisabled()) {
        return;
    }
    std::vector<CodeRange> ranges;
    dl_iterate_phdr(appendMainExecutableCodeRanges, &ranges);
    size_t remappedSize = 0;
    for (auto&& range : ranges) {
        if (int error = remapToHugePages(range); error != 0) {
            LOGV2_WARNING(11520100,
                          "Failed to remap code to huge pages",
                          "error"_attr = errorMessage(posixError(error)),
                          "address"_attr = unsignedHex(range.begin),
                          "size"_attr = range.end - range.begin);
            continue;
        }
        remappedSize += range.end - range.begin;
    }
    LOGV2(11520101,
          "Remapped code to huge pages",
          "numRanges"_attr = ranges.size(),
          "remappedSize"_attr = remappedSize);
}

}  // namespace
}  // namespace mongo
