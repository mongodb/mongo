/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

/**
 * Stacktraces using libunwind for non-Windows OSes.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/stacktrace.h"

#include <cstdlib>
#include <dlfcn.h>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <iostream>
#include <libunwind.h>
#include <limits.h>
#include <string>
#include <sys/utsname.h>

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"
#include "mongo/util/version.h"

namespace mongo {

namespace {

using namespace fmt::literals;

/// Maximum number of stack frames to appear in a backtrace.
constexpr int maxBackTraceFrames = 100;

/// Maximum size of a function name and signature. This is an arbitrary limit we impose to avoid
/// having to malloc while backtracing.
constexpr int maxSymbolLen = 512;

/// Optional string containing extra unwinding information.  Should take the form of a
/// JSON document.
std::string* soMapJson = NULL;

/**
 * Returns the "basename" of a path.  The returned StringData is valid until the data referenced
 * by "path" goes out of scope or mutates.
 *
 * E.g., for "/foo/bar/my.txt", returns "my.txt".
 */
StringData getBaseName(StringData path) {
    size_t lastSlash = path.rfind('/');
    if (lastSlash == std::string::npos)
        return path;
    return path.substr(lastSlash + 1);
}

struct frame {
    frame() {
        symbol[0] = '\0';
        filename[0] = '\0';
    }

    void* address = nullptr;
    // Some getFrames() implementations can fill this in, some can't.
    constexpr static ssize_t kOffsetUnknown = -1;
    ssize_t offset = kOffsetUnknown;
    char symbol[maxSymbolLen];

    // Filled in by completeFrame().
    void* baseAddress = nullptr;
    char filename[PATH_MAX];
};

void strncpyTerm(char* dst, const char* src, size_t len) {
    strncpy(dst, src, len);
    // In case src is longer than dst, ensure dst is terminated.
    dst[len - 1] = '\0';
}

void completeFrame(frame* pFrame, std::ostream& logStream) {
    Dl_info dlinfo{};
    if (dladdr(pFrame->address, &dlinfo)) {
        pFrame->baseAddress = dlinfo.dli_fbase;
        if (pFrame->offset == frame::kOffsetUnknown) {
            pFrame->offset =
                static_cast<char*>(pFrame->address) - static_cast<char*>(dlinfo.dli_saddr);
        }
        if (dlinfo.dli_fname) {
            strncpyTerm(pFrame->filename, dlinfo.dli_fname, sizeof(pFrame->filename));
        }

        // Don't overwrite pFrame->symbol if getFrames() has already figured out the symbol name.
        if (dlinfo.dli_sname && !*pFrame->symbol) {
            strncpyTerm(pFrame->symbol, dlinfo.dli_sname, sizeof(pFrame->symbol));
        }
    } else {
        logStream << "error: unable to obtain symbol information for function "
                  << "{:p}\n"_format(pFrame->address) << std::endl;
    }

    // Don't log errors, they're expected for C functions in the stack, like "main".
    int status;
    if (char* demangled_name = abi::__cxa_demangle(pFrame->symbol, nullptr, nullptr, &status)) {
        strncpyTerm(pFrame->symbol, demangled_name, sizeof(pFrame->symbol));
        free(demangled_name);
    }
}

#define MONGO_UNWIND_CHECK(FUNC, ...)                                                 \
    do {                                                                              \
        unw_status = FUNC(__VA_ARGS__);                                               \
        if (unw_status < 0) {                                                         \
            logStream << "error from {}: {}"_format(#FUNC, unw_strerror(unw_status)); \
            return 0;                                                                 \
        }                                                                             \
    } while (0)

size_t getFrames(frame* frames, bool fromSignal, std::ostream& logStream) {
    size_t nFrames = 0;
    frame* pFrame = frames;
    unw_cursor_t cursor;
    unw_context_t context;
    int unw_status;

    // Initialize cursor to current frame for local unwinding.
    MONGO_UNWIND_CHECK(unw_getcontext, &context);
    MONGO_UNWIND_CHECK(unw_init_local2, &cursor, &context, fromSignal ? UNW_INIT_SIGNAL_FRAME : 0);

    // Inspect this frame, then step to calling frame, and repeat.
    while (nFrames < maxBackTraceFrames) {
        unw_word_t pc;
        MONGO_UNWIND_CHECK(unw_get_reg, &cursor, UNW_REG_IP, &pc);
        if (pc == 0) {
            break;
        }

        pFrame->address = reinterpret_cast<void*>(pc);
        unw_word_t offset;
        if ((unw_status = unw_get_proc_name(
                 &cursor, pFrame->symbol, sizeof(pFrame->symbol), &offset)) != 0) {
            logStream << "error: unable to obtain symbol name for function "
                      << "{:p}: {}\n"_format(pFrame->address, unw_strerror(unw_status))
                      << std::endl;
        } else {
            pFrame->offset = static_cast<size_t>(offset);
        }

        nFrames++;
        pFrame++;

        MONGO_UNWIND_CHECK(unw_step, &cursor);
        if (unw_status == 0) {
            // Finished.
            break;
        }
    }

    return nFrames;
}

/**
 * Prints a stack backtrace for the current thread to the specified ostream.
 *
 * Does not malloc, does not throw.
 *
 * The format of the backtrace is:
 *
 * ----- BEGIN BACKTRACE -----
 * JSON backtrace
 * Human-readable backtrace
 * -----  END BACKTRACE  -----
 *
 * The JSON backtrace will be a JSON object with a "backtrace" field, and optionally others.
 * The "backtrace" field is an array, whose elements are frame objects.  A frame object has a
 * "b" field, which is the base-address of the library or executable containing the symbol, and
 * an "o" field, which is the offset into said library or executable of the symbol.
 *
 * The JSON backtrace may optionally contain additional information useful to a backtrace
 * analysis tool.  For example, on Linux it contains a subobject named "somap", describing
 * the objects referenced in the "b" fields of the "backtrace" list.
 *
 * Notes for future refinements: we have a goal of making this malloc-free so it's signal-safe. This
 * huge stack-allocated structure reduces the need for malloc, at the risk of a stack overflow while
 * trying to print a stack trace, which would make life very hard for us when we're debugging
 * crashes for customers. It would also be bad to crash from stack overflow when printing backtraces
 * on demand (SERVER-33445).
 *
 * A better idea is to get rid of the "frame" struct idea. Instead, iterate stack frames with
 * unw_step while printing the JSON stack, then reset the cursor and iterate with unw_step again
 * while printing the human-readable stack. Then there's no need for a large amount of storage.
 *
 * @param os    ostream& to receive printed stack backtrace
 */
void printStackTraceInternal(std::ostream& os, bool fromSignal) {
    frame frames[maxBackTraceFrames];

    ////////////////////////////////////////////////////////////
    // Get the backtrace.
    ////////////////////////////////////////////////////////////
    size_t nFrames = getFrames(frames, fromSignal, os);
    if (!nFrames) {
        // getFrames logged an error to "os".
        return;
    }

    for (size_t i = 0; i < nFrames; i++) {
        completeFrame(&frames[i], os);
    }

    os << std::hex << std::uppercase;
    for (size_t i = 0; i < nFrames; ++i) {
        os << ' ' << frames[i].address;
    }

    os << "\n----- BEGIN BACKTRACE -----\n";

    ////////////////////////////////////////////////////////////
    // Display the JSON backtrace
    ////////////////////////////////////////////////////////////

    os << "{\"backtrace\":[";
    for (size_t i = 0; i < nFrames; ++i) {
        if (i)
            os << ',';
        frame* pFrame = &frames[i];
        const uintptr_t fileOffset =
            static_cast<char*>(pFrame->address) - static_cast<char*>(pFrame->baseAddress);
        os << "{\"b\":\"" << pFrame->baseAddress << "\",\"o\":\"" << fileOffset;
        if (*pFrame->symbol) {
            os << "\",\"s\":\"" << pFrame->symbol;
        }
        os << "\"}";
    }
    os << ']';

    if (soMapJson)
        os << ",\"processInfo\":" << *soMapJson;
    os << "}\n";

    ////////////////////////////////////////////////////////////
    // Display the human-readable trace
    ////////////////////////////////////////////////////////////

    for (size_t i = 0; i < nFrames; ++i) {
        frame* pFrame = &frames[i];
        os << ' ' << getBaseName(pFrame->filename);
        if (pFrame->baseAddress) {
            os << '(';
            if (*pFrame->symbol) {
                os << pFrame->symbol;
            }

            if (pFrame->offset != frame::kOffsetUnknown) {
                os << "+0x" << pFrame->offset;
            }

            os << ')';
        }
        os << " [0x" << reinterpret_cast<uintptr_t>(pFrame->address) << ']' << std::endl;
    }

    os << std::dec << std::nouppercase;
    os << "-----  END BACKTRACE  -----" << std::endl;
}

}  // namespace

void printStackTrace(std::ostream& os) {
    printStackTraceInternal(os, false /* fromSignal */);
}

void printStackTraceFromSignal(std::ostream& os) {
    printStackTraceInternal(os, true /* fromSignal */);
}

// From here down, a copy of stacktrace_posix.cpp.
namespace {

void addOSComponentsToSoMap(BSONObjBuilder* soMap);

/**
 * Builds the "soMapJson" string, which is a JSON encoding of various pieces of information
 * about a running process, including the map from load addresses to shared objects loaded at
 * those addresses.
 */
MONGO_INITIALIZER(ExtractSOMap)(InitializerContext*) {
    BSONObjBuilder soMap;

    auto&& vii = VersionInfoInterface::instance(VersionInfoInterface::NotEnabledAction::kFallback);
    soMap << "mongodbVersion" << vii.version();
    soMap << "gitVersion" << vii.gitVersion();
    soMap << "compiledModules" << vii.modules();

    struct utsname unameData;
    if (!uname(&unameData)) {
        BSONObjBuilder unameBuilder(soMap.subobjStart("uname"));
        unameBuilder << "sysname" << unameData.sysname << "release" << unameData.release
                     << "version" << unameData.version << "machine" << unameData.machine;
    }
    addOSComponentsToSoMap(&soMap);
    soMapJson = new std::string(soMap.done().jsonString(Strict));
    return Status::OK();
}
}  // namespace

}  // namespace mongo

#if defined(__linux__)

#include <elf.h>
#include <link.h>

namespace mongo {
namespace {

/**
 * Rounds a byte offset up to the next highest offset that is aligned with an ELF Word.
 */
size_t roundUpToElfWordAlignment(size_t offset) {
    static const size_t elfWordSizeBytes = sizeof(ElfW(Word));
    return (offset + (elfWordSizeBytes - 1)) & ~(elfWordSizeBytes - 1);
}

/**
 * Returns the size in bytes of an ELF note entry with the given header.
 */
size_t getNoteSizeBytes(const ElfW(Nhdr) & noteHeader) {
    return sizeof(noteHeader) + roundUpToElfWordAlignment(noteHeader.n_namesz) +
        roundUpToElfWordAlignment(noteHeader.n_descsz);
}

/**
 * Returns true of the given ELF program header refers to a runtime-readable segment.
 */
bool isSegmentMappedReadable(const ElfW(Phdr) & phdr) {
    return phdr.p_flags & PF_R;
}

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
        soInfo->append("buildId", toHex(noteDescBegin, noteHeader.n_descsz));
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
    if (phdr.p_memsz < sizeof(ElfW(Ehdr)))
        return;

    // Segment includes beginning of file and is large enough to hold the ELF header
    ElfW(Ehdr) eHeader;
    memcpy(&eHeader, reinterpret_cast<const char*>(info.dlpi_addr) + phdr.p_vaddr, sizeof(eHeader));

    std::string quotedFileName = "\"" + str::escape(info.dlpi_name) + "\"";

    if (memcmp(&eHeader.e_ident[0], ELFMAG, SELFMAG)) {
        warning() << "Bad ELF magic number in image of " << quotedFileName;
        return;
    }

#if defined(__ELF_NATIVE_CLASS)
#define ARCH_BITS __ELF_NATIVE_CLASS
#else  //__ELF_NATIVE_CLASS
#if defined(__x86_64__) || defined(__aarch64__)
#define ARCH_BITS 64
#elif defined(__arm__)
#define ARCH_BITS 32
#else
#error Unknown target architecture.
#endif  //__aarch64__
#endif  //__ELF_NATIVE_CLASS

#define MKELFCLASS(N) _MKELFCLASS(N)
#define _MKELFCLASS(N) ELFCLASS##N
    if (eHeader.e_ident[EI_CLASS] != MKELFCLASS(ARCH_BITS)) {
        warning() << "Expected elf file class of " << quotedFileName << " to be "
                  << MKELFCLASS(ARCH_BITS) << "(" << ARCH_BITS << "-bit), but found "
                  << int(eHeader.e_ident[4]);
        return;
    }

#undef ARCH_BITS

    if (eHeader.e_ident[EI_VERSION] != EV_CURRENT) {
        warning() << "Wrong ELF version in " << quotedFileName << ".  Expected " << EV_CURRENT
                  << " but found " << int(eHeader.e_ident[EI_VERSION]);
        return;
    }

    soInfo->append("elfType", eHeader.e_type);

    switch (eHeader.e_type) {
        case ET_EXEC:
            break;
        case ET_DYN:
            return;
        default:
            warning() << "Surprised to find " << quotedFileName << " is ELF file of type "
                      << eHeader.e_type;
            return;
    }

    soInfo->append("b", integerToHex(phdr.p_vaddr));
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
        soInfo.append("b", integerToHex(ElfW(Addr)(info->dlpi_addr)));
    if (info->dlpi_name && *info->dlpi_name)
        soInfo.append("path", info->dlpi_name);

    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) & phdr(info->dlpi_phdr[i]);
        if (!isSegmentMappedReadable(phdr))
            continue;
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
    BSONArrayBuilder soList(soMap->subarrayStart("somap"));
    dl_iterate_phdr(outputSOInfo, &soList);
    soList.done();
}

}  // namespace

}  // namespace mongo

#elif defined(__APPLE__) && defined(__MACH__)

#include <mach-o/dyld.h>
#include <mach-o/ldsyms.h>
#include <mach-o/loader.h>

namespace mongo {
namespace {
const char* lcNext(const char* lcCurr) {
    const load_command* cmd = reinterpret_cast<const load_command*>(lcCurr);
    return lcCurr + cmd->cmdsize;
}

uint32_t lcType(const char* lcCurr) {
    const load_command* cmd = reinterpret_cast<const load_command*>(lcCurr);
    return cmd->cmd;
}

template <typename SegmentCommandType>
bool maybeAppendLoadAddr(BSONObjBuilder* soInfo, const SegmentCommandType* segmentCommand) {
    if (StringData(SEG_TEXT) != segmentCommand->segname) {
        return false;
    }
    *soInfo << "vmaddr" << integerToHex(segmentCommand->vmaddr);
    return true;
}

void addOSComponentsToSoMap(BSONObjBuilder* soMap) {
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
        soInfo << "machType" << header->filetype;
        soInfo << "b" << integerToHex(reinterpret_cast<intptr_t>(header));
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
                    soInfo << "buildId" << toHex(uuidCmd->uuid, 16);
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
}  // namespace
}  // namespace mongo
#else
namespace mongo {
namespace {
void addOSComponentsToSoMap(BSONObjBuilder* soMap) {}
}  // namespace
}  // namespace mongo
#endif
