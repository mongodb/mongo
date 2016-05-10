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

#include "mongo/util/stacktrace.h"

#include <cstdlib>
#include <dlfcn.h>
#include <iostream>
#include <string>
#include <sys/utsname.h>

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/version.h"

#if defined(MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE)
#include <execinfo.h>
#elif defined(__sun)
#include <ucontext.h>
#endif

namespace mongo {

namespace {
/// Maximum number of stack frames to appear in a backtrace.
const int maxBackTraceFrames = 100;

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

// All platforms we build on have execinfo.h and we use backtrace() directly, with one exception
#if defined(MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE)
using ::backtrace;

// On Solaris 10, there is no execinfo.h, so we need to emulate it.
// Solaris 11 has execinfo.h, and this code doesn't get used.
#elif defined(__sun)
class WalkcontextCallback {
public:
    WalkcontextCallback(uintptr_t* array, int size)
        : _position(0), _count(size), _addresses(array) {}

    // This callback function is called from C code, and so must not throw exceptions
    //
    static int callbackFunction(uintptr_t address,
                                int signalNumber,
                                WalkcontextCallback* thisContext) {
        if (thisContext->_position < thisContext->_count) {
            thisContext->_addresses[thisContext->_position++] = address;
            return 0;
        }
        return 1;
    }
    int getCount() const {
        return static_cast<int>(_position);
    }

private:
    size_t _position;
    size_t _count;
    uintptr_t* _addresses;
};

typedef int (*WalkcontextCallbackFunc)(uintptr_t address, int signalNumber, void* thisContext);

int backtrace(void** array, int size) {
    WalkcontextCallback walkcontextCallback(reinterpret_cast<uintptr_t*>(array), size);
    ucontext_t context;
    if (getcontext(&context) != 0) {
        return 0;
    }
    int wcReturn = walkcontext(
        &context,
        reinterpret_cast<WalkcontextCallbackFunc>(WalkcontextCallback::callbackFunction),
        static_cast<void*>(&walkcontextCallback));
    if (wcReturn == 0) {
        return walkcontextCallback.getCount();
    }
    return 0;
}
#else
// On unsupported platforms, we print an error instead of printing a stacktrace.
#define MONGO_NO_BACKTRACE
#endif

}  // namespace

#if defined(MONGO_NO_BACKTRACE)
void printStackTrace(std::ostream& os) {
    os << "This platform does not support printing stacktraces" << std::endl;
}

#else
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
 * @param os    ostream& to receive printed stack backtrace
 */
void printStackTrace(std::ostream& os) {
    static const char unknownFileName[] = "???";
    void* addresses[maxBackTraceFrames];
    Dl_info dlinfoForFrames[maxBackTraceFrames];

    ////////////////////////////////////////////////////////////
    // Get the backtrace addresses.
    ////////////////////////////////////////////////////////////

    const int addressCount = backtrace(addresses, maxBackTraceFrames);
    if (addressCount == 0) {
        const int err = errno;
        os << "Unable to collect backtrace addresses (errno: " << err << ' ' << strerror(err) << ')'
           << std::endl;
        return;
    }

    ////////////////////////////////////////////////////////////
    // Collect symbol information for each backtrace address.
    ////////////////////////////////////////////////////////////

    os << std::hex << std::uppercase << '\n';
    for (int i = 0; i < addressCount; ++i) {
        Dl_info& dlinfo(dlinfoForFrames[i]);
        if (!dladdr(addresses[i], &dlinfo)) {
            dlinfo.dli_fname = unknownFileName;
            dlinfo.dli_fbase = NULL;
            dlinfo.dli_sname = NULL;
            dlinfo.dli_saddr = NULL;
        }
        os << ' ' << addresses[i];
    }

    os << "\n----- BEGIN BACKTRACE -----\n";

    ////////////////////////////////////////////////////////////
    // Display the JSON backtrace
    ////////////////////////////////////////////////////////////

    os << "{\"backtrace\":[";
    for (int i = 0; i < addressCount; ++i) {
        const Dl_info& dlinfo = dlinfoForFrames[i];
        const uintptr_t fileOffset = uintptr_t(addresses[i]) - uintptr_t(dlinfo.dli_fbase);
        if (i)
            os << ',';
        os << "{\"b\":\"" << uintptr_t(dlinfo.dli_fbase) << "\",\"o\":\"" << fileOffset;
        if (dlinfo.dli_sname) {
            os << "\",\"s\":\"" << dlinfo.dli_sname;
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
    for (int i = 0; i < addressCount; ++i) {
        Dl_info& dlinfo(dlinfoForFrames[i]);
        os << ' ';
        if (dlinfo.dli_fbase) {
            os << getBaseName(dlinfo.dli_fname) << '(';
            if (dlinfo.dli_sname) {
                const uintptr_t offset = uintptr_t(addresses[i]) - uintptr_t(dlinfo.dli_saddr);
                os << dlinfo.dli_sname << "+0x" << offset;
            } else {
                const uintptr_t offset = uintptr_t(addresses[i]) - uintptr_t(dlinfo.dli_fbase);
                os << "+0x" << offset;
            }
            os << ')';
        } else {
            os << unknownFileName;
        }
        os << " [" << addresses[i] << ']' << std::endl;
    }

    os << std::dec << std::nouppercase;
    os << "-----  END BACKTRACE  -----" << std::endl;
}

#endif

namespace {

void addOSComponentsToSoMap(BSONObjBuilder* soMap);

/**
 * Builds the "soMapJson" string, which is a JSON encoding of various pieces of information
 * about a running process, including the map from load addresses to shared objects loaded at
 * those addresses.
 */
MONGO_INITIALIZER(ExtractSOMap)(InitializerContext*) {
    BSONObjBuilder soMap;
    soMap << "mongodbVersion" << versionString;
    soMap << "gitVersion" << gitVersion();
    soMap << "compiledModules" << compiledModules();
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

    std::string quotedFileName = "\"" + escape(info.dlpi_name) + "\"";

    if (memcmp(&eHeader.e_ident[0], ELFMAG, SELFMAG)) {
        warning() << "Bad ELF magic number in image of " << quotedFileName;
        return;
    }

#define MKELFCLASS(N) _MKELFCLASS(N)
#define _MKELFCLASS(N) ELFCLASS##N
    if (eHeader.e_ident[EI_CLASS] != MKELFCLASS(__ELF_NATIVE_CLASS)) {
        warning() << "Expected elf file class of " << quotedFileName << " to be "
                  << MKELFCLASS(__ELF_NATIVE_CLASS) << "(" << __ELF_NATIVE_CLASS
                  << "-bit), but found " << int(eHeader.e_ident[4]);
        return;
    }

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
}  // namepace
}  // namespace mongo
#else
namespace mongo {
namespace {
void addOSComponentsToSoMap(BSONObjBuilder* soMap) {}
}  // namepace
}  // namespace mongo
#endif
