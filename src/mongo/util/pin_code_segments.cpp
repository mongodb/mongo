/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <vector>

#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/initialize_server_global_state_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/hex.h"
#include "mongo/util/pin_code_segments_params_gen.h"

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
        if (auto f = phdr.p_flags; f & PF_R && f & PF_X && !(f & PF_W)) {
            // Code segments typically have read and execute permissions, but not writable
            // permissions. Meanwhile, data segments, which we want to ignore, typically have
            // read, write and execute permissions.
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
                "memSize"_attr = segment.memSize);
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
