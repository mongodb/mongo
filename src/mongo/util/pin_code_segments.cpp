// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/logv2/log.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/pin_code_segments_params_gen.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <elf.h>
#include <link.h>

#include <fmt/format.h>
#include <sys/mman.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {

using ElfWEhdr = ElfW(Ehdr);
using ElfWHalf = ElfW(Half);
using ElfWPhdr = ElfW(Phdr);

struct CodeSegment {
    void* addr;
    size_t memSize;
};

int appendCodeSegments(dl_phdr_info* info, size_t size, void* data) {
    auto segments = reinterpret_cast<std::vector<CodeSegment>*>(data);
    for (ElfWHalf i = 0; i < info->dlpi_phnum; ++i) {
        const ElfWPhdr& phdr(info->dlpi_phdr[i]);
        if (phdr.p_type != PT_LOAD) {
            // Code segments must be of LOAD type.
            continue;
        }
        const auto f = phdr.p_flags;
        if (!(f & PF_R)) {
            // Code segments have read permissions so exclude segments that do not.
            continue;
        }
        if (!(f & PF_X)) {
            // Code segments have execute permissions so exclude segments that do not.
            continue;
        }
        if (f & PF_W) {
            // Code segments don't have write permissions so exclude segments that do.
            continue;
        }
        segments->push_back(
            {reinterpret_cast<void*>(info->dlpi_addr + phdr.p_vaddr), phdr.p_memsz});
    }
    return 0;
}

std::vector<CodeSegment> getCodeSegments() {
    std::vector<CodeSegment> segments;
    dl_iterate_phdr(appendCodeSegments, &segments);
    return segments;
}

MONGO_INITIALIZER(PinCodeSegments)(InitializerContext*) {
    if (!gLockCodeSegmentsInMemory) {
        return;
    }
    LOGV2(7394303, "Pinning code segments");
    size_t lockedMemSize = 0;
    auto codeSegments = getCodeSegments();
    for (auto&& segment : codeSegments) {
        if (mlock(segment.addr, segment.memSize) != 0) {
            auto ec = lastSystemError();
            LOGV2_FATAL(
                7394301,
                "Failed to lock code segment, ensure system ulimits are properly configured",
                "error"_attr = errorMessage(ec),
                "address"_attr = unsignedHex(reinterpret_cast<uintptr_t>(segment.addr)),
                "memSize"_attr = segment.memSize,
                "memLockedSoFar"_attr = lockedMemSize);
        }
        lockedMemSize += segment.memSize;
    }
    LOGV2(7394302,
          "Successfully locked code segments into memory",
          "numSegments"_attr = codeSegments.size(),
          "totalLockedMemSize"_attr = lockedMemSize);
}

}  // namespace
}  // namespace mongo
