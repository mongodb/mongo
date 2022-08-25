/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/util/stacktrace_somap.h"

#include <climits>
#include <cstdlib>
#include <fmt/format.h>
#include <string>

#if defined(__linux__)
#include <elf.h>
#include <link.h>
#elif defined(__APPLE__) && defined(__MACH__)
#include <mach-o/dyld.h>
#include <mach-o/ldsyms.h>
#include <mach-o/loader.h>
#endif

#if !defined(_WIN32)
#include <sys/utsname.h>
#endif

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/jsobj.h"
#include "mongo/logv2/log.h"
#include "mongo/util/hex.h"
#include "mongo/util/str.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


// Given `#define A aaa` and `#define B bbb`, `TOKEN_CAT(A, B)` evaluates to `aaabbb`.
#define TOKEN_CAT(a, b) TOKEN_CAT_PRIMITIVE(a, b)
#define TOKEN_CAT_PRIMITIVE(a, b) a##b

namespace mongo {

namespace {

void addUnameToSoMap(BSONObjBuilder* soMap) {
#if !defined(_WIN32)
    struct utsname unameData;
    if (!uname(&unameData)) {
        BSONObjBuilder unameBuilder(soMap->subobjStart("uname"));
        unameBuilder << "sysname" << unameData.sysname << "release" << unameData.release
                     << "version" << unameData.version << "machine" << unameData.machine;
    }
#endif
}

#if defined(__linux__)

#if defined(__ELF_NATIVE_CLASS)  // determine ARCH_BITS
#define ARCH_BITS __ELF_NATIVE_CLASS
#elif defined(__x86_64__) || defined(__aarch64__)
#define ARCH_BITS 64
#elif defined(__arm__)
#define ARCH_BITS 32
#else
#error Unknown target architecture.
#endif  // determine ARCH_BITS

#define ARCH_ELFCLASS TOKEN_CAT(ELFCLASS, ARCH_BITS)

/**
 * Processes an ELF Phdr for a NOTE segment, updating "soInfo".
 *
 * Looks for the GNU Build ID NOTE, and adds a buildId field to soInfo if it finds one.
 */
void processNoteSegment(const dl_phdr_info& info, const ElfW(Phdr) & phdr, BSONObjBuilder* soInfo) {
#ifdef NT_GNU_BUILD_ID
    const char* const notesBegin = reinterpret_cast<const char*>(info.dlpi_addr) + phdr.p_vaddr;
    const char* const notesEnd = notesBegin + phdr.p_memsz;
    ElfW(Nhdr) noteHeader;
    // Returns the size in bytes of an ELF note entry with the given header.
    auto roundUpToElfWordAlignment = [](size_t offset) -> size_t {
        static const size_t elfWordSizeBytes = sizeof(ElfW(Word));
        return (offset + (elfWordSizeBytes - 1)) & ~(elfWordSizeBytes - 1);
    };
    auto getNoteSizeBytes = [&](const ElfW(Nhdr) & noteHeader) -> size_t {
        return sizeof(noteHeader) + roundUpToElfWordAlignment(noteHeader.n_namesz) +
            roundUpToElfWordAlignment(noteHeader.n_descsz);
    };
    for (const char* notesCurr = notesBegin; (notesCurr + sizeof(noteHeader)) < notesEnd;
         notesCurr += getNoteSizeBytes(noteHeader)) {
        memcpy(&noteHeader, notesCurr, sizeof(noteHeader));
        if (noteHeader.n_type != NT_GNU_BUILD_ID)
            continue;
        const char* const noteNameBegin = notesCurr + sizeof(noteHeader);
        if (StringData(noteNameBegin, noteHeader.n_namesz - 1) != ELF_NOTE_GNU) {
            continue;
        }
        const char* const noteDescBegin =
            noteNameBegin + roundUpToElfWordAlignment(noteHeader.n_namesz);
        soInfo->append("buildId", hexblob::encode(noteDescBegin, noteHeader.n_descsz));
    }
#endif
}

/**
 * Processes an ELF Phdr for a LOAD segment, updating "soInfo".
 *
 * The goal of this operation is to find out if the current object is an executable or a shared
 * object, by looking for the LOAD segment that maps the first several bytes of the file (the
 * ELF header).  If it's an executable, this method updates soInfo with the load address of the
 * segment
 */
void processLoadSegment(const dl_phdr_info& info, const ElfW(Phdr) & phdr, BSONObjBuilder* soInfo) {
    if (phdr.p_offset)
        return;

    ElfW(Ehdr) eHeader;
    if (phdr.p_memsz < sizeof(eHeader))
        return;

    // Segment includes beginning of file and is large enough to hold the ELF header
    memcpy(&eHeader, (char*)(info.dlpi_addr + phdr.p_vaddr), sizeof(eHeader));

    const char* filename = info.dlpi_name;

    if (memcmp(&eHeader.e_ident[EI_MAG0], ELFMAG, SELFMAG) != 0) {
        LOGV2_WARNING(23842,
                      "Bad ELF magic number",
                      "filename"_attr = filename,
                      "magic"_attr = hexdump((const char*)&eHeader.e_ident[EI_MAG0], SELFMAG),
                      "magicExpected"_attr = hexdump(ELFMAG, SELFMAG));
        return;
    }

    if (uint8_t elfClass = eHeader.e_ident[EI_CLASS]; elfClass != ARCH_ELFCLASS) {
        auto elfClassStr = [](uint8_t c) -> std::string {
            switch (c) {
                case ELFCLASS32:
                    return "ELFCLASS32";
                case ELFCLASS64:
                    return "ELFCLASS64";
            }
            return format(FMT_STRING("[elfClass unknown: {}]"), c);
        };
        LOGV2_WARNING(23843,
                      "Unexpected ELF class (i.e. bit width)",
                      "filename"_attr = filename,
                      "elfClass"_attr = elfClassStr(elfClass),
                      "elfClassExpected"_attr = elfClassStr(ARCH_ELFCLASS));
        return;
    }

    if (uint32_t elfVersion = eHeader.e_ident[EI_VERSION]; elfVersion != EV_CURRENT) {
        LOGV2_WARNING(23844,
                      "Wrong ELF version",
                      "filename"_attr = filename,
                      "elfVersion"_attr = elfVersion,
                      "elfVersionExpected"_attr = EV_CURRENT);
        return;
    }

    uint16_t elfType = eHeader.e_type;
    soInfo->append("elfType", elfType);

    switch (elfType) {
        case ET_EXEC:
            break;
        case ET_DYN:
            return;
        default:
            LOGV2_WARNING(
                23845, "Unexpected ELF type", "filename"_attr = filename, "elfType"_attr = elfType);
            return;
    }

    soInfo->append("b", unsignedHex(phdr.p_vaddr));
}

/**
 * Callback that processes an ELF object linked into the current address space.
 *
 * Used by dl_iterate_phdr in ExtractSOMap, below, to build up the list of linked
 * objects.
 *
 * Each entry built by an invocation of ths function may have the following fields:
 * * "b", the base address at which an object is loaded.
 * * "path", the path on the file system to the object.
 * * "buildId", the GNU Build ID of the object.
 * * "elfType", the ELF type of the object, typically 2 or 3 (executable or SO).
 *
 * At post-processing time, the buildId field can be used to identify the file containing
 * debug symbols for objects loaded at the given "laodAddr", which in turn can be used with
 * the "backtrace" displayed in printStackTrace to get detailed unwind information.
 */
int outputSOInfo(dl_phdr_info* info, size_t sz, void* data) {
    BSONObjBuilder soInfo(reinterpret_cast<BSONArrayBuilder*>(data)->subobjStart());
    if (info->dlpi_addr)
        soInfo.append("b", unsignedHex(ElfW(Addr)(info->dlpi_addr)));
    if (info->dlpi_name && *info->dlpi_name)
        soInfo.append("path", info->dlpi_name);

    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) & phdr(info->dlpi_phdr[i]);
        if (!(phdr.p_flags & PF_R))
            continue;  // skip non-readable segments
        switch (phdr.p_type) {
            case PT_NOTE:
                processNoteSegment(*info, phdr, &soInfo);
                break;
            case PT_LOAD:
                processLoadSegment(*info, phdr, &soInfo);
                break;
            default:
                break;
        }
    }
    return 0;
}

void addOSComponentsToSoMap(BSONObjBuilder* soMap) {
    addUnameToSoMap(soMap);
    BSONArrayBuilder soList(soMap->subarrayStart("somap"));
    dl_iterate_phdr(outputSOInfo, &soList);
    soList.done();
}

#elif defined(__APPLE__) && defined(__MACH__)

void addOSComponentsToSoMap(BSONObjBuilder* soMap) {
    addUnameToSoMap(soMap);
    auto lcNext = [](const char* lcCurr) -> const char* {
        return lcCurr + reinterpret_cast<const load_command*>(lcCurr)->cmdsize;
    };
    auto lcType = [](const char* lcCurr) -> uint32_t {
        return reinterpret_cast<const load_command*>(lcCurr)->cmd;
    };
    auto maybeAppendLoadAddr = [](BSONObjBuilder* soInfo, const auto* segmentCommand) -> bool {
        if (StringData(SEG_TEXT) != segmentCommand->segname) {
            return false;
        }
        *soInfo << "vmaddr" << unsignedHex(segmentCommand->vmaddr);
        return true;
    };
    const uint32_t numImages = _dyld_image_count();
    BSONArrayBuilder soList(soMap->subarrayStart("somap"));
    for (uint32_t i = 0; i < numImages; ++i) {
        BSONObjBuilder soInfo(soList.subobjStart());
        const char* name = _dyld_get_image_name(i);
        if (name)
            soInfo << "path" << name;
        const mach_header* header = _dyld_get_image_header(i);
        if (!header)
            continue;
        size_t headerSize;
        if (header->magic == MH_MAGIC) {
            headerSize = sizeof(mach_header);
        } else if (header->magic == MH_MAGIC_64) {
            headerSize = sizeof(mach_header_64);
        } else {
            continue;
        }
        soInfo << "machType" << static_cast<int32_t>(header->filetype);
        soInfo << "b" << unsignedHex(reinterpret_cast<uintptr_t>(header));
        const char* const loadCommandsBegin = reinterpret_cast<const char*>(header) + headerSize;
        const char* const loadCommandsEnd = loadCommandsBegin + header->sizeofcmds;

        // Search the "load command" data in the Mach object for the entry encoding the UUID of the
        // object, and for the __TEXT segment. Adding the "vmaddr" field of the __TEXT segment load
        // command of an executable or dylib to an offset in that library provides an address
        // suitable to passing to atos or llvm-symbolizer for symbolization.
        //
        // See, for example, http://lldb.llvm.org/symbolication.html.
        bool foundTextSegment = false;
        for (const char* lcCurr = loadCommandsBegin; lcCurr < loadCommandsEnd;
             lcCurr = lcNext(lcCurr)) {
            switch (lcType(lcCurr)) {
                case LC_UUID: {
                    const auto uuidCmd = reinterpret_cast<const uuid_command*>(lcCurr);
                    soInfo << "buildId" << hexblob::encode(uuidCmd->uuid, 16);
                    break;
                }
                case LC_SEGMENT_64:
                    if (!foundTextSegment) {
                        foundTextSegment = maybeAppendLoadAddr(
                            &soInfo, reinterpret_cast<const segment_command_64*>(lcCurr));
                    }
                    break;
                case LC_SEGMENT:
                    if (!foundTextSegment) {
                        foundTextSegment = maybeAppendLoadAddr(
                            &soInfo, reinterpret_cast<const segment_command*>(lcCurr));
                    }
                    break;
            }
        }
    }
}

#else  // unknown OS

void addOSComponentsToSoMap(BSONObjBuilder* soMap) {}

#endif  // unknown OS

/**
 * Used to build the "processInfo" field of the stacktrace JSON object. It's loaded with
 * information about a running process, including the map from load addresses to shared
 * objects loaded at those addresses.
 */
BSONObj buildObj() {
    BSONObjBuilder soMap;
    auto&& vii = VersionInfoInterface::instance(VersionInfoInterface::NotEnabledAction::kFallback);
    soMap << "mongodbVersion" << vii.version();
    soMap << "gitVersion" << vii.gitVersion();
    soMap << "compiledModules" << vii.modules();
    addOSComponentsToSoMap(&soMap);
    return soMap.obj();
}

SharedObjectMapInfo& mutableGlobalSharedObjectMapInfo() {
    static auto& p = *new SharedObjectMapInfo(buildObj());
    return p;
}

MONGO_INITIALIZER(ExtractSOMap)(InitializerContext*) {
    // Call buildObj() again now that there is better VersionInfo.
    mutableGlobalSharedObjectMapInfo().setObj(buildObj());
}

const bool dummyToForceEarlyInitializationOfSharedObjectMapInfo = [] {
    mutableGlobalSharedObjectMapInfo();
    return true;
}();

}  // namespace

SharedObjectMapInfo::SharedObjectMapInfo(BSONObj obj) : _obj(std::move(obj)) {}

const BSONObj& SharedObjectMapInfo::obj() const {
    return _obj;
}

void SharedObjectMapInfo::setObj(BSONObj obj) {
    _obj = std::move(obj);
}

const SharedObjectMapInfo& globalSharedObjectMapInfo() {
    // This file internally has a non-const object, but only exposes a const reference
    // to it to the public API. We do this to support stacktraces that might occur
    // before the "ExtractSOMap" MONGO_INITIALIZER above.
    return mutableGlobalSharedObjectMapInfo();
}

}  // namespace mongo
