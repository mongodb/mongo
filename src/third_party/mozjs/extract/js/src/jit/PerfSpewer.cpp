/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/Printf.h"

#if defined(JS_ION_PERF) && defined(XP_UNIX)
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#if defined(JS_ION_PERF) && defined(XP_LINUX) && !defined(ANDROID) && \
    defined(__GLIBC__)
#  include <dlfcn.h>
#  include <sys/syscall.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define gettid() static_cast<pid_t>(syscall(__NR_gettid))
#endif

#if defined(JS_ION_PERF) && (defined(ANDROID) || defined(XP_DARWIN))
#  include <limits.h>
#  include <stdlib.h>
#  include <unistd.h>
char* get_current_dir_name() {
  char* buffer = (char*)malloc(PATH_MAX * sizeof(char));
  if (buffer == nullptr) {
    return nullptr;
  }

  if (getcwd(buffer, PATH_MAX) == nullptr) {
    free(buffer);
    return nullptr;
  }

  return buffer;
}
#endif

#if defined(JS_ION_PERF) && defined(XP_DARWIN)
#  include <pthread.h>
#  include <unistd.h>

pid_t gettid_pthread() {
  uint64_t tid;
  if (pthread_threadid_np(nullptr, &tid) != 0) {
    return 0;
  }
  // Truncate the tid to 32 bits. macOS thread IDs are usually small enough.
  // And even if we do end up truncating, it doesn't matter much for Jitdump
  // as long as the process ID is correct.
  return pid_t(tid);
}
#  define gettid() gettid_pthread()
#endif

#include "jit/PerfSpewer.h"

#include <atomic>

#include "jit/Jitdump.h"
#include "jit/JitSpewer.h"
#include "jit/LIR.h"
#include "jit/MIR.h"
#include "js/ColumnNumber.h"  // JS::LimitedColumnNumberOneOrigin, JS::ColumnNumberOffset
#include "js/JitCodeAPI.h"
#include "js/Printf.h"
#include "vm/BytecodeUtil.h"
#include "vm/MutexIDs.h"

#ifdef XP_WIN
#  include <windef.h>
#  include <codecvt>
#  include <evntprov.h>
#  include <locale>
#  include <string>
#  include <windows.h>

const GUID PROVIDER_JSCRIPT9 = {
    0x57277741,
    0x3638,
    0x4a4b,
    {0xbd, 0xba, 0x0a, 0xc6, 0xe4, 0x5d, 0xa5, 0x6c}};
const EVENT_DESCRIPTOR MethodLoad = {0x9, 0x0, 0x0, 0x4, 0xa, 0x1, 0x1};

static REGHANDLE sETWRegistrationHandle = NULL;

static std::atomic<bool> etwCollection = false;
#endif

using namespace js;
using namespace js::jit;

enum class PerfModeType { None, Function, Source, IR, IROperands };

static std::atomic<bool> geckoProfiling = false;
static std::atomic<PerfModeType> PerfMode = PerfModeType::None;

// Mutex to guard access to the profiler vectors and jitdump file if perf
// profiling is enabled.
static js::Mutex PerfMutex(mutexid::PerfSpewer);

static PersistentRooted<GCVector<JitCode*, 0, js::SystemAllocPolicy>>
    jitCodeVector;
static ProfilerJitCodeVector profilerData;

static bool IsGeckoProfiling() { return geckoProfiling; }
#ifdef JS_ION_PERF
static UniqueChars spew_dir;
static FILE* JitDumpFilePtr = nullptr;
static void* mmap_address = nullptr;
static bool IsPerfProfiling() { return JitDumpFilePtr != nullptr; }
#endif

AutoLockPerfSpewer::AutoLockPerfSpewer() { PerfMutex.lock(); }

AutoLockPerfSpewer::~AutoLockPerfSpewer() { PerfMutex.unlock(); }

#ifdef JS_ION_PERF
static uint64_t GetMonotonicTimestamp() {
  using mozilla::TimeStamp;
#  ifdef XP_LINUX
  return TimeStamp::Now().RawClockMonotonicNanosecondsSinceBoot();
#  elif XP_WIN
  return TimeStamp::Now().RawQueryPerformanceCounterValue().value();
#  elif XP_DARWIN
  return TimeStamp::Now().RawMachAbsoluteTimeNanoseconds();
#  else
  MOZ_CRASH("no timestamp");
#  endif
}

// values are from /usr/include/elf.h
static uint32_t GetMachineEncoding() {
#  if defined(JS_CODEGEN_X86)
  return 3;  // EM_386
#  elif defined(JS_CODEGEN_X64)
  return 62;  // EM_X86_64
#  elif defined(JS_CODEGEN_ARM)
  return 40;  // EM_ARM
#  elif defined(JS_CODEGEN_ARM64)
  return 183;  // EM_AARCH64
#  elif defined(JS_CODEGEN_MIPS32)
  return 8;  // EM_MIPS
#  elif defined(JS_CODEGEN_MIPS64)
  return 8;  // EM_MIPS
#  else
  return 0;  // Unsupported
#  endif
}

static void WriteToJitDumpFile(const void* addr, uint32_t size,
                               AutoLockPerfSpewer& lock) {
  MOZ_RELEASE_ASSERT(JitDumpFilePtr);
  size_t rv = fwrite(addr, 1, size, JitDumpFilePtr);
  MOZ_RELEASE_ASSERT(rv == size);
}

static void WriteJitDumpDebugEntry(uint64_t addr, const char* filename,
                                   uint32_t lineno,
                                   JS::LimitedColumnNumberOneOrigin colno,
                                   AutoLockPerfSpewer& lock) {
  JitDumpDebugEntry entry = {addr, lineno, colno.oneOriginValue()};
  WriteToJitDumpFile(&entry, sizeof(entry), lock);
  WriteToJitDumpFile(filename, strlen(filename) + 1, lock);
}

static void writeJitDumpHeader(AutoLockPerfSpewer& lock) {
  JitDumpHeader header = {};
  header.magic = 0x4A695444;
  header.version = 1;
  header.total_size = sizeof(header);
  header.elf_mach = GetMachineEncoding();
  header.pad1 = 0;
  header.pid = getpid();
  header.timestamp = GetMonotonicTimestamp();
  header.flags = 0;

  WriteToJitDumpFile(&header, sizeof(header), lock);
}

static bool openJitDump() {
  if (JitDumpFilePtr) {
    return true;
  }
  AutoLockPerfSpewer lock;

  const ssize_t bufferSize = 256;
  char filenameBuffer[bufferSize];

  // We want to write absolute filenames into the debug info or else we get the
  // filenames in the perf report
  if (getenv("PERF_SPEW_DIR")) {
    char* env_dir = getenv("PERF_SPEW_DIR");
    if (env_dir[0] == '/') {
      spew_dir = JS_smprintf("%s", env_dir);
    } else {
      const char* dir = get_current_dir_name();
      if (!dir) {
        fprintf(stderr, "couldn't get current dir name\n");
        return false;
      }
      spew_dir = JS_smprintf("%s/%s", dir, env_dir);
      free((void*)dir);
    }
  } else {
    fprintf(stderr, "Please define PERF_SPEW_DIR as an output directory.\n");
    return false;
  }

  if (snprintf(filenameBuffer, bufferSize, "%s/jit-%d.dump", spew_dir.get(),
               getpid()) >= bufferSize) {
    return false;
  }

  MOZ_ASSERT(!JitDumpFilePtr);

  int fd = open(filenameBuffer, O_CREAT | O_TRUNC | O_RDWR, 0666);
  JitDumpFilePtr = fdopen(fd, "w+");

  if (!JitDumpFilePtr) {
    return false;
  }

#  ifdef XP_LINUX
  // We need to mmap the jitdump file for perf to find it.
  long page_size = sysconf(_SC_PAGESIZE);
  int prot = PROT_READ | PROT_EXEC;
  // The mmap call fails on some Android devices if PROT_EXEC is specified, and
  // it does not appear to be required by simpleperf, so omit it on Android.
#    ifdef ANDROID
  prot &= ~PROT_EXEC;
#    endif
  mmap_address = mmap(nullptr, page_size, prot, MAP_PRIVATE, fd, 0);
  if (mmap_address == MAP_FAILED) {
    PerfMode = PerfModeType::None;
    return false;
  }
#  endif

  writeJitDumpHeader(lock);
  return true;
}

static void CheckPerf() {
  static bool PerfChecked = false;

  if (!PerfChecked) {
    const char* env = getenv("IONPERF");
    if (env == nullptr) {
      PerfMode = PerfModeType::None;
      fprintf(stderr,
              "Warning: JIT perf reporting requires IONPERF set to \"func\" "
              ", \"src\" or \"ir\". ");
      fprintf(stderr, "Perf mapping will be deactivated.\n");
    } else if (!strcmp(env, "src")) {
      PerfMode = PerfModeType::Source;
    } else if (!strcmp(env, "ir")) {
      PerfMode = PerfModeType::IR;
    } else if (!strcmp(env, "ir-ops")) {
#  ifdef JS_JITSPEW
      PerfMode = PerfModeType::IROperands;
#  else
      fprintf(stderr,
              "Warning: IONPERF=ir-ops requires --enable-jitspew to be "
              "enabled, defaulting to IONPERF=ir\n");
      PerfMode = PerfModeType::IR;
#  endif
    } else if (!strcmp(env, "func")) {
      PerfMode = PerfModeType::Function;
    } else {
      fprintf(stderr, "Use IONPERF=func to record at function granularity\n");
      fprintf(stderr,
              "Use IONPERF=ir to record and annotate assembly with IR\n");
      fprintf(stderr,
              "Use IONPERF=src to record and annotate assembly with source, if "
              "available locally\n");
      exit(0);
    }

    if (PerfMode != PerfModeType::None) {
      if (openJitDump()) {
        PerfChecked = true;
        return;
      }

      fprintf(stderr, "Failed to open perf map file.  Disabling IONPERF.\n");
      PerfMode = PerfModeType::None;
    }
    PerfChecked = true;
  }
}
#endif

#ifdef XP_WIN
void NTAPI ETWEnableCallback(LPCGUID aSourceId, ULONG aIsEnabled, UCHAR aLevel,
                             ULONGLONG aMatchAnyKeyword,
                             ULONGLONG aMatchAllKeyword,
                             PEVENT_FILTER_DESCRIPTOR aFilterData,
                             PVOID aCallbackContext) {
  // This is called on a CRT worker thread. This means this might race with
  // our main thread, but that is okay.
  etwCollection = aIsEnabled;
  PerfMode = aIsEnabled ? PerfModeType::Function : PerfModeType::None;
}

void RegisterETW() {
  static bool sHasRegisteredETW = false;
  if (!sHasRegisteredETW) {
    if (getenv("ETW_ENABLED")) {
      EventRegister(&PROVIDER_JSCRIPT9,  // GUID that identifies the provider
                    ETWEnableCallback,   // Callback for enabling collection
                    NULL,                // Context not used
                    &sETWRegistrationHandle  // Used when calling EventWrite
                                             // and EventUnregister
      );
    }
    sHasRegisteredETW = true;
  }
}
#endif

/* static */
void PerfSpewer::Init() {
#ifdef JS_ION_PERF
  CheckPerf();
#endif
#ifdef XP_WIN
  RegisterETW();
#endif
}

static void DisablePerfSpewer(AutoLockPerfSpewer& lock) {
  fprintf(stderr, "Warning: Disabling PerfSpewer.");

  geckoProfiling = false;
#ifdef XP_WIN
  etwCollection = false;
#endif
  PerfMode = PerfModeType::None;
#ifdef JS_ION_PERF
  long page_size = sysconf(_SC_PAGESIZE);
  munmap(mmap_address, page_size);
  fclose(JitDumpFilePtr);
  JitDumpFilePtr = nullptr;
#endif
}

void js::jit::ResetPerfSpewer(bool enabled) {
  AutoLockPerfSpewer lock;

  profilerData.clear();
  jitCodeVector.clear();
  geckoProfiling = enabled;
}

static JS::JitCodeRecord* CreateProfilerEntry(AutoLockPerfSpewer& lock) {
  if (!IsGeckoProfiling()) {
    return nullptr;
  }

  if (!profilerData.growBy(1)) {
    DisablePerfSpewer(lock);
    return nullptr;
  }
  return &profilerData.back();
}

static JS::JitCodeIRInfo* CreateProfilerIREntry(JS::JitCodeRecord* record,
                                                AutoLockPerfSpewer& lock) {
  if (!record) {
    return nullptr;
  }

  if (!record->irInfo.growBy(1)) {
    DisablePerfSpewer(lock);
    return nullptr;
  }
  return &record->irInfo.back();
}

static JS::JitCodeSourceInfo* CreateProfilerSourceEntry(
    JS::JitCodeRecord* record, AutoLockPerfSpewer& lock) {
  if (!record) {
    return nullptr;
  }

  if (!record->sourceInfo.growBy(1)) {
    DisablePerfSpewer(lock);
    return nullptr;
  }
  return &record->sourceInfo.back();
}

JS::JitOpcodeDictionary::JitOpcodeDictionary() {
  MOZ_ASSERT(JS_IsInitialized());

#define COPY_JSOP_OPCODE(name, ...)                     \
  if (!baselineDictionary.append(JS_smprintf(#name))) { \
    return;                                             \
  }
  FOR_EACH_OPCODE(COPY_JSOP_OPCODE)
#undef COPY_JSOP_OPCODE

#define COPY_LIR_OPCODE(name, ...)                 \
  if (!ionDictionary.append(JS_smprintf(#name))) { \
    return;                                        \
  }
  LIR_OPCODE_LIST(COPY_LIR_OPCODE)
#undef COPY_LIR_OPCODE

#define COPY_CACHEIR_OPCODE(name, ...)            \
  if (!icDictionary.append(JS_smprintf(#name))) { \
    return;                                       \
  }
  CACHE_IR_OPS(COPY_CACHEIR_OPCODE)
#undef COPY_CACHEIR_OPCODE
}

// API to access JitCode data for the Gecko Profiler.
void JS::JitCodeIterator::getDataForIndex(size_t IteratorIndex) {
  if (IteratorIndex >= profilerData.length()) {
    data = nullptr;
  } else {
    data = &profilerData[IteratorIndex];
  }
}

JS::JitCodeIterator::JitCodeIterator() : iteratorIndex(0) {
  MOZ_ASSERT(JS_IsInitialized());
  PerfMutex.lock();
  getDataForIndex(0);
}

JS::JitCodeIterator::~JitCodeIterator() { PerfMutex.unlock(); }

static bool PerfSrcEnabled() {
  return PerfMode == PerfModeType::Source || geckoProfiling;
}

#ifdef JS_JITSPEW
static bool PerfIROpsEnabled() { return PerfMode == PerfModeType::IROperands; }
#endif

static bool PerfIREnabled() {
  return (PerfMode == PerfModeType::IROperands) ||
         (PerfMode == PerfModeType::IR) || geckoProfiling;
}

static bool PerfFuncEnabled() {
  return PerfMode == PerfModeType::Function || geckoProfiling;
}

bool js::jit::PerfEnabled() {
  return PerfSrcEnabled() || PerfIREnabled() || PerfFuncEnabled();
}

void InlineCachePerfSpewer::recordInstruction(MacroAssembler& masm,
                                              CacheOp op) {
  if (!PerfIREnabled()) {
    return;
  }
  AutoLockPerfSpewer lock;

  if (!opcodes_.emplaceBack(masm.currentOffset(), static_cast<unsigned>(op))) {
    opcodes_.clear();
    DisablePerfSpewer(lock);
  }
}

#define CHECK_RETURN(x)      \
  if (!(x)) {                \
    AutoLockPerfSpewer lock; \
    DisablePerfSpewer(lock); \
    return;                  \
  }

void IonPerfSpewer::recordInstruction(MacroAssembler& masm, LInstruction* ins) {
  if (!PerfIREnabled() && !PerfSrcEnabled()) {
    return;
  }

  LNode::Opcode op = ins->op();
  UniqueChars opcodeStr;

  jsbytecode* bytecodepc = nullptr;
  if (MDefinition* mir = ins->mirRaw()) {
    bytecodepc = mir->trackedSite()->pc();
  }

#ifdef JS_JITSPEW
  if (PerfIROpsEnabled()) {
    Sprinter buf;
    CHECK_RETURN(buf.init());
    buf.put(LIRCodeName(op));
    ins->printOperands(buf);
    opcodeStr = buf.release();
  }
#endif
  if (!opcodes_.emplaceBack(masm.currentOffset(), static_cast<unsigned>(op),
                            opcodeStr, bytecodepc)) {
    opcodes_.clear();
    AutoLockPerfSpewer lock;
    DisablePerfSpewer(lock);
  }
}

#ifdef JS_JITSPEW
static void PrintStackValue(JSContext* cx, StackValue* stackVal,
                            CompilerFrameInfo& frame, Sprinter& buf) {
  switch (stackVal->kind()) {
    /****** Constant ******/
    case StackValue::Constant: {
      js::Value constantVal = stackVal->constant();
      if (constantVal.isInt32()) {
        buf.printf("%d", constantVal.toInt32());
      } else if (constantVal.isObjectOrNull()) {
        buf.printf("obj:%p", constantVal.toObjectOrNull());
      } else if (constantVal.isString()) {
        buf.put("str:");
        buf.putString(cx, constantVal.toString());
      } else if (constantVal.isNumber()) {
        buf.printf("num:%f", constantVal.toNumber());
      } else if (constantVal.isSymbol()) {
        buf.put("sym:");
        constantVal.toSymbol()->dump(buf);
      } else {
        buf.printf("raw:%" PRIx64, constantVal.asRawBits());
      }
    } break;
    /****** Register ******/
    case StackValue::Register: {
      Register reg = stackVal->reg().payloadOrValueReg();
      buf.put(reg.name());
    } break;
    /****** Stack ******/
    case StackValue::Stack:
      buf.put("stack");
      break;
    /****** ThisSlot ******/
    case StackValue::ThisSlot: {
#  ifdef JS_HAS_HIDDEN_SP
      buf.put("this");
#  else
      Address addr = frame.addressOfThis();
      buf.printf("this:%s(%d)", addr.base.name(), addr.offset);
#  endif
    } break;
    /****** LocalSlot ******/
    case StackValue::LocalSlot:
      buf.printf("local:%u", stackVal->localSlot());
      break;
    /****** ArgSlot ******/
    case StackValue::ArgSlot:
      buf.printf("arg:%u", stackVal->argSlot());
      break;

    default:
      MOZ_CRASH("Unexpected kind");
      break;
  }
}
#endif

void BaselinePerfSpewer::recordInstruction(JSContext* cx, MacroAssembler& masm,
                                           jsbytecode* pc,
                                           CompilerFrameInfo& frame) {
  if (!PerfIREnabled() && !PerfSrcEnabled()) {
    return;
  }

  JSOp op = JSOp(*pc);
  UniqueChars opcodeStr;

#ifdef JS_JITSPEW
  if (PerfIROpsEnabled()) {
    JSScript* script = frame.script;
    unsigned numOperands = js::StackUses(op, pc);

    Sprinter buf(cx);
    CHECK_RETURN(buf.init());
    buf.put(js::CodeName(op));

    switch (op) {
      case JSOp::SetName:
      case JSOp::SetGName:
      case JSOp::BindName:
      case JSOp::BindGName:
      case JSOp::GetName:
      case JSOp::GetGName: {
        // Emit the name used for these ops
        Rooted<PropertyName*> name(cx, script->getName(pc));
        buf.put(" ");
        buf.putString(cx, name);
      } break;
      default:
        break;
    }

    // Output should be "JSOp (operand1), (operand2), ..."
    for (unsigned i = 1; i <= numOperands; i++) {
      buf.put(" (");
      StackValue* stackVal = frame.peek(-int(i));
      PrintStackValue(cx, stackVal, frame, buf);

      if (i < numOperands) {
        buf.put("),");
      } else {
        buf.put(")");
      }
    }
    opcodeStr = buf.release();
  }
#endif

  if (!opcodes_.emplaceBack(masm.currentOffset(), static_cast<unsigned>(op),
                            opcodeStr, pc)) {
    opcodes_.clear();
    AutoLockPerfSpewer lock;
    DisablePerfSpewer(lock);
  }
}

const char* BaselinePerfSpewer::CodeName(unsigned op) {
  return js::CodeName(static_cast<JSOp>(op));
}

const char* BaselineInterpreterPerfSpewer::CodeName(unsigned op) {
  return js::CodeName(static_cast<JSOp>(op));
}

const char* IonPerfSpewer::CodeName(unsigned op) {
  return js::jit::LIRCodeName(static_cast<LNode::Opcode>(op));
}
const char* InlineCachePerfSpewer::CodeName(unsigned op) {
  return js::jit::CacheIRCodeName(static_cast<CacheOp>(op));
}

void PerfSpewer::CollectJitCodeInfo(UniqueChars& function_name, JitCode* code,
                                    JS::JitCodeRecord* profilerRecord,
                                    AutoLockPerfSpewer& lock) {
  // Hold the JitCode objects here so they are not GC'd during profiling.
  if (IsGeckoProfiling()) {
    if (!jitCodeVector.append(code)) {
      DisablePerfSpewer(lock);
    }
  }

  CollectJitCodeInfo(function_name, reinterpret_cast<void*>(code->raw()),
                     code->instructionsSize(), profilerRecord, lock);
}

void PerfSpewer::CollectJitCodeInfo(UniqueChars& function_name, void* code_addr,
                                    uint64_t code_size,
                                    JS::JitCodeRecord* profilerRecord,
                                    AutoLockPerfSpewer& lock) {
#ifdef JS_ION_PERF
  static uint64_t codeIndex = 1;

  if (IsPerfProfiling()) {
    JitDumpLoadRecord record = {};

    record.header.id = JIT_CODE_LOAD;
    record.header.total_size =
        sizeof(record) + strlen(function_name.get()) + 1 + code_size;
    record.header.timestamp = GetMonotonicTimestamp();
    record.pid = getpid();
    record.tid = gettid();
    record.vma = uint64_t(code_addr);
    record.code_addr = uint64_t(code_addr);
    record.code_size = code_size;
    record.code_index = codeIndex++;

    WriteToJitDumpFile(&record, sizeof(record), lock);
    WriteToJitDumpFile(function_name.get(), strlen(function_name.get()) + 1,
                       lock);
    WriteToJitDumpFile(code_addr, code_size, lock);
  }
#endif
#ifdef XP_WIN
  if (etwCollection) {
    void* scriptContextId = NULL;
    uint32_t flags = 0;
    uint64_t map = 0;
    uint64_t assembly = 0;
    uint32_t line_col = 0;
    uint32_t method = 0;

    int name_len = strlen(function_name.get());
    std::wstring name(name_len + 1, '\0');
    if (MultiByteToWideChar(CP_UTF8, 0, function_name.get(), name_len,
                            name.data(), name.size()) == 0) {
      DisablePerfSpewer(lock);
      return;
    }

    EVENT_DATA_DESCRIPTOR EventData[10];

    EventDataDescCreate(&EventData[0], &scriptContextId, sizeof(PVOID));
    EventDataDescCreate(&EventData[1], &code_addr, sizeof(PVOID));
    EventDataDescCreate(&EventData[2], &code_size, sizeof(unsigned __int64));
    EventDataDescCreate(&EventData[3], &method, sizeof(uint32_t));
    EventDataDescCreate(&EventData[4], &flags, sizeof(const unsigned short));
    EventDataDescCreate(&EventData[5], &map, sizeof(const unsigned short));
    EventDataDescCreate(&EventData[6], &assembly, sizeof(unsigned __int64));
    EventDataDescCreate(&EventData[7], &line_col, sizeof(const unsigned int));
    EventDataDescCreate(&EventData[8], &line_col, sizeof(const unsigned int));
    EventDataDescCreate(&EventData[9], name.c_str(),
                        sizeof(wchar_t) * (name.length() + 1));

    ULONG result = EventWrite(
        sETWRegistrationHandle,  // From EventRegister
        &MethodLoad,             // EVENT_DESCRIPTOR generated from the manifest
        (ULONG)10,               // Size of the array of EVENT_DATA_DESCRIPTORs
        EventData  // Array of descriptors that contain the event data
    );

    if (result != ERROR_SUCCESS) {
      DisablePerfSpewer(lock);
      return;
    }
  }
#endif

  if (IsGeckoProfiling()) {
    profilerRecord->instructionSize = code_size;
    profilerRecord->code_addr = uint64_t(code_addr);
    profilerRecord->functionName = std::move(function_name);
  }
}

void PerfSpewer::recordOffset(MacroAssembler& masm, const char* msg) {
  if (!PerfIREnabled()) {
    return;
  }

  UniqueChars offsetStr = DuplicateString(msg);
  if (!opcodes_.emplaceBack(masm.currentOffset(), offsetStr)) {
    opcodes_.clear();
    AutoLockPerfSpewer lock;
    DisablePerfSpewer(lock);
  }
}

void PerfSpewer::saveJitCodeIRInfo(JitCode* code,
                                   JS::JitCodeRecord* profilerRecord,
                                   AutoLockPerfSpewer& lock) {
#ifdef JS_ION_PERF
  static uint32_t filenameCounter = 0;
  UniqueChars scriptFilename;
  FILE* scriptFile = nullptr;

  if (IsPerfProfiling()) {
    scriptFilename = JS_smprintf("%s/jitdump-script-%u.%u.txt", spew_dir.get(),
                                 filenameCounter++, getpid());
    scriptFile = fopen(scriptFilename.get(), "w");
    if (!scriptFile) {
      DisablePerfSpewer(lock);
      return;
    }

    JitDumpDebugRecord debug_record = {};
    uint64_t n_records = opcodes_.length();

    debug_record.header.id = JIT_CODE_DEBUG_INFO;
    debug_record.header.total_size =
        sizeof(debug_record) + n_records * (sizeof(JitDumpDebugEntry) +
                                            strlen(scriptFilename.get()) + 1);
    debug_record.header.timestamp = GetMonotonicTimestamp();
    debug_record.code_addr = uint64_t(code->raw());
    debug_record.nr_entry = n_records;

    WriteToJitDumpFile(&debug_record, sizeof(debug_record), lock);
  }
#endif

  if (profilerRecord) {
    profilerRecord->tier = GetTier();
  }

  for (size_t i = 0; i < opcodes_.length(); i++) {
    OpcodeEntry& entry = opcodes_[i];
#ifdef JS_ION_PERF
    if (IsPerfProfiling()) {
      // If a string was recorded for this offset, use that instead.
      if (entry.str) {
        fprintf(scriptFile, "%s\n", entry.str.get());
      } else {
        fprintf(scriptFile, "%s\n", CodeName(entry.opcode));
      }
      uint64_t addr = uint64_t(code->raw()) + entry.offset;
      uint64_t lineno = i + 1;
      WriteJitDumpDebugEntry(addr, scriptFilename.get(), lineno,
                             JS::LimitedColumnNumberOneOrigin(), lock);
    }
#endif

    if (JS::JitCodeIRInfo* irInfo =
            CreateProfilerIREntry(profilerRecord, lock)) {
      irInfo->offset = entry.offset;
      irInfo->opcode = entry.opcode;
      // Profiler API now owns this string, if defined.
      irInfo->str = std::move(entry.str);
    }
  }
  opcodes_.clear();

#ifdef JS_ION_PERF
  if (IsPerfProfiling()) {
    fclose(scriptFile);
  }
#endif
}

void PerfSpewer::saveJitCodeSourceInfo(JSScript* script, JitCode* code,
                                       JS::JitCodeRecord* profilerRecord,
                                       AutoLockPerfSpewer& lock) {
  const char* filename = script->filename();
  if (!filename) {
    return;
  }

#ifdef JS_ION_PERF
  bool perfProfiling = IsPerfProfiling();

  if (perfProfiling) {
    JitDumpDebugRecord debug_record = {};

    uint64_t n_records = 0;
    for (OpcodeEntry& entry : opcodes_) {
      if (entry.bytecodepc) {
        n_records++;
      }
    }

    debug_record.header.id = JIT_CODE_DEBUG_INFO;
    debug_record.header.total_size =
        sizeof(debug_record) +
        n_records * (sizeof(JitDumpDebugEntry) + strlen(filename) + 1);
    debug_record.header.timestamp = GetMonotonicTimestamp();
    debug_record.code_addr = uint64_t(code->raw());
    debug_record.nr_entry = n_records;

    WriteToJitDumpFile(&debug_record, sizeof(debug_record), lock);
  }
#endif
  uint32_t lineno = 0;
  JS::LimitedColumnNumberOneOrigin colno;

  for (OpcodeEntry& entry : opcodes_) {
    jsbytecode* pc = entry.bytecodepc;
    if (!pc) {
      continue;
    }
    // We could probably make this a bit faster by caching the previous pc
    // offset, but it currently doesn't seem noticeable when testing.
    lineno = PCToLineNumber(script, pc, &colno);

    if (JS::JitCodeSourceInfo* srcInfo =
            CreateProfilerSourceEntry(profilerRecord, lock)) {
      srcInfo->offset = entry.offset;
      srcInfo->lineno = lineno;
      srcInfo->colno = colno;
      srcInfo->filename = JS_smprintf("%s", filename);
    }

#ifdef JS_ION_PERF
    if (perfProfiling) {
      WriteJitDumpDebugEntry(uint64_t(code->raw()) + entry.offset, filename,
                             lineno, colno, lock);
    }
#endif
  }
}

static UniqueChars GetFunctionDesc(const char* tierName, JSContext* cx,
                                   JSScript* script,
                                   const char* stubName = nullptr) {
  MOZ_ASSERT(script && tierName && cx);
  UniqueChars funName;
  if (script->function() && script->function()->maybePartialDisplayAtom()) {
    funName = AtomToPrintableString(
        cx, script->function()->maybePartialDisplayAtom());
  }

  if (stubName) {
    return JS_smprintf("%s: %s : %s (%s:%u:%u)", tierName, stubName,
                       funName ? funName.get() : "*", script->filename(),
                       script->lineno(), script->column().oneOriginValue());
  }
  return JS_smprintf("%s: %s (%s:%u:%u)", tierName,
                     funName ? funName.get() : "*", script->filename(),
                     script->lineno(), script->column().oneOriginValue());
}

void PerfSpewer::saveDebugInfo(JSScript* script, JitCode* code,
                               JS::JitCodeRecord* profilerRecord,
                               AutoLockPerfSpewer& lock) {
  MOZ_ASSERT(code);
  if (PerfIREnabled()) {
    saveJitCodeIRInfo(code, profilerRecord, lock);
  } else if (PerfSrcEnabled() && script) {
    saveJitCodeSourceInfo(script, code, profilerRecord, lock);
  }
}

void PerfSpewer::saveProfile(JitCode* code, UniqueChars& desc,
                             JSScript* script) {
  MOZ_ASSERT(PerfEnabled());
  MOZ_ASSERT(code && desc);
  AutoLockPerfSpewer lock;
  JS::JitCodeRecord* profilerRecord = CreateProfilerEntry(lock);

  saveDebugInfo(script, code, profilerRecord, lock);
  CollectJitCodeInfo(desc, code, profilerRecord, lock);
}

IonICPerfSpewer::IonICPerfSpewer(jsbytecode* pc) {
  if (!opcodes_.emplaceBack(pc)) {
    AutoLockPerfSpewer lock;
    opcodes_.clear();
    DisablePerfSpewer(lock);
  }
}

void IonICPerfSpewer::saveJitCodeSourceInfo(JSScript* script, JitCode* code,
                                            JS::JitCodeRecord* profilerRecord,
                                            AutoLockPerfSpewer& lock) {
#ifdef JS_ION_PERF
  MOZ_ASSERT(opcodes_.length() == 1);
  jsbytecode* pc = opcodes_[0].bytecodepc;

  if (!pc) {
    return;
  }

  const char* filename = script->filename();
  if (!filename) {
    return;
  }

  if (!IsPerfProfiling()) {
    return;
  }

  JitDumpDebugRecord debug_record = {};
  uint64_t n_records = 1;

  debug_record.header.id = JIT_CODE_DEBUG_INFO;
  debug_record.header.total_size =
      sizeof(debug_record) +
      n_records * (sizeof(JitDumpDebugEntry) + strlen(filename) + 1);

  debug_record.header.timestamp = GetMonotonicTimestamp();
  debug_record.code_addr = uint64_t(code->raw());
  debug_record.nr_entry = n_records;

  WriteToJitDumpFile(&debug_record, sizeof(debug_record), lock);

  uint32_t lineno;
  JS::LimitedColumnNumberOneOrigin colno;
  lineno = PCToLineNumber(script, pc, &colno);

  WriteJitDumpDebugEntry(uint64_t(code->raw()), filename, lineno, colno, lock);
#endif
}

void IonICPerfSpewer::saveProfile(JSContext* cx, JSScript* script,
                                  JitCode* code, const char* stubName) {
  if (!PerfEnabled()) {
    return;
  }
  UniqueChars desc = GetFunctionDesc("IonIC", cx, script, stubName);
  PerfSpewer::saveProfile(code, desc, script);
}

void BaselineICPerfSpewer::saveProfile(JitCode* code, const char* stubName) {
  if (!PerfEnabled()) {
    return;
  }
  UniqueChars desc = JS_smprintf("BaselineIC: %s", stubName);
  PerfSpewer::saveProfile(code, desc, nullptr);
}

void BaselinePerfSpewer::saveProfile(JSContext* cx, JSScript* script,
                                     JitCode* code) {
  if (!PerfEnabled()) {
    return;
  }
  UniqueChars desc = GetFunctionDesc("Baseline", cx, script);
  PerfSpewer::saveProfile(code, desc, script);
}

void BaselineInterpreterPerfSpewer::saveProfile(JitCode* code) {
  if (!PerfEnabled()) {
    return;
  }

  enum class SpewKind { Uninitialized, SingleSym, MultiSym };

  // Check which type of Baseline Interpreter Spew is requested.
  static SpewKind kind = SpewKind::Uninitialized;
  if (kind == SpewKind::Uninitialized) {
    if (getenv("IONPERF_SINGLE_BLINTERP")) {
      kind = SpewKind::SingleSym;
    } else {
      kind = SpewKind::MultiSym;
    }
  }

  // For SingleSym, just emit one "BaselineInterpreter" symbol
  // and emit the opcodes as IR if IONPERF=ir is used.
  if (kind == SpewKind::SingleSym) {
    UniqueChars desc = DuplicateString("BaselineInterpreter");
    PerfSpewer::saveProfile(code, desc, nullptr);
    return;
  }

  // For MultiSym, split up each opcode into its own symbol.
  // No IR is emitted in this case, so we can skip PerfSpewer::saveProfile.
  MOZ_ASSERT(kind == SpewKind::MultiSym);
  for (size_t i = 1; i < opcodes_.length(); i++) {
    uintptr_t base = uintptr_t(code->raw()) + opcodes_[i - 1].offset;
    uintptr_t size = opcodes_[i].offset - opcodes_[i - 1].offset;

    UniqueChars rangeName;
    if (opcodes_[i - 1].str) {
      rangeName = JS_smprintf("BlinterpOp: %s", opcodes_[i - 1].str.get());
    } else {
      rangeName =
          JS_smprintf("BlinterpOp: %s", CodeName(opcodes_[i - 1].opcode));
    }

    // If rangeName is empty, we probably went OOM.
    if (!rangeName) {
      AutoLockPerfSpewer lock;
      DisablePerfSpewer(lock);
      return;
    }

    MOZ_ASSERT(base + size <=
               uintptr_t(code->raw()) + code->instructionsSize());
    CollectPerfSpewerJitCodeProfile(base, size, rangeName.get());
  }
}

void BaselineInterpreterPerfSpewer::recordOffset(MacroAssembler& masm,
                                                 JSOp op) {
  if (!PerfEnabled()) {
    return;
  }

  if (!opcodes_.emplaceBack(masm.currentOffset(), unsigned(op))) {
    opcodes_.clear();
    AutoLockPerfSpewer lock;
    DisablePerfSpewer(lock);
  }
}

void BaselineInterpreterPerfSpewer::recordOffset(MacroAssembler& masm,
                                                 const char* name) {
  if (!PerfEnabled()) {
    return;
  }

  UniqueChars desc = DuplicateString(name);
  if (!opcodes_.emplaceBack(masm.currentOffset(), desc)) {
    opcodes_.clear();
    AutoLockPerfSpewer lock;
    DisablePerfSpewer(lock);
  }
}

void IonPerfSpewer::saveProfile(JSContext* cx, JSScript* script,
                                JitCode* code) {
  if (!PerfEnabled()) {
    return;
  }
  UniqueChars desc = GetFunctionDesc("Ion", cx, script);
  PerfSpewer::saveProfile(code, desc, script);
}

void js::jit::CollectPerfSpewerJitCodeProfile(JitCode* code, const char* msg) {
  if (!code || !PerfEnabled()) {
    return;
  }

  size_t size = code->instructionsSize();
  if (size > 0) {
    AutoLockPerfSpewer lock;

    JS::JitCodeRecord* profilerRecord = CreateProfilerEntry(lock);
    UniqueChars desc = JS_smprintf("%s", msg);
    PerfSpewer::CollectJitCodeInfo(desc, code, profilerRecord, lock);
  }
}

void js::jit::CollectPerfSpewerJitCodeProfile(uintptr_t base, uint64_t size,
                                              const char* msg) {
  if (!PerfEnabled()) {
    return;
  }

  if (size > 0) {
    AutoLockPerfSpewer lock;

    JS::JitCodeRecord* profilerRecord = CreateProfilerEntry(lock);
    UniqueChars desc = JS_smprintf("%s", msg);
    PerfSpewer::CollectJitCodeInfo(desc, reinterpret_cast<void*>(base), size,
                                   profilerRecord, lock);
  }
}

void js::jit::CollectPerfSpewerWasmMap(uintptr_t base, uintptr_t size,
                                       const char* filename,
                                       const char* annotation) {
  if (size == 0U || !PerfEnabled()) {
    return;
  }
  AutoLockPerfSpewer lock;

  JS::JitCodeRecord* profilerRecord = CreateProfilerEntry(lock);
  UniqueChars desc = JS_smprintf("%s: Function %s", filename, annotation);
  PerfSpewer::CollectJitCodeInfo(desc, reinterpret_cast<void*>(base),
                                 uint64_t(size), profilerRecord, lock);
}

void js::jit::CollectPerfSpewerWasmFunctionMap(uintptr_t base, uintptr_t size,
                                               const char* filename,
                                               unsigned lineno,
                                               const char* funcName) {
  if (size == 0U || !PerfEnabled()) {
    return;
  }
  AutoLockPerfSpewer lock;

  JS::JitCodeRecord* profilerRecord = CreateProfilerEntry(lock);
  UniqueChars desc =
      JS_smprintf("%s:%u: Function %s", filename, lineno, funcName);
  PerfSpewer::CollectJitCodeInfo(desc, reinterpret_cast<void*>(base),
                                 uint64_t(size), profilerRecord, lock);
}

void js::jit::PerfSpewerRangeRecorder::appendEntry(UniqueChars& desc) {
  if (!ranges.append(std::make_pair(masm.currentOffset(), std::move(desc)))) {
    AutoLockPerfSpewer lock;
    DisablePerfSpewer(lock);
    ranges.clear();
  }
}

void js::jit::PerfSpewerRangeRecorder::recordOffset(const char* name) {
  if (!PerfEnabled()) {
    return;
  }
  UniqueChars desc = DuplicateString(name);
  appendEntry(desc);
}

void js::jit::PerfSpewerRangeRecorder::recordVMWrapperOffset(const char* name) {
  if (!PerfEnabled()) {
    return;
  }

  UniqueChars desc = JS_smprintf("VMWrapper: %s", name);
  appendEntry(desc);
}

void js::jit::PerfSpewerRangeRecorder::recordOffset(const char* name,
                                                    JSContext* cx,
                                                    JSScript* script) {
  if (!PerfEnabled()) {
    return;
  }
  UniqueChars desc = GetFunctionDesc(name, cx, script);
  appendEntry(desc);
}

void js::jit::PerfSpewerRangeRecorder::collectRangesForJitCode(JitCode* code) {
  if (!PerfEnabled() || ranges.empty()) {
    return;
  }

  uintptr_t basePtr = uintptr_t(code->raw());
  uintptr_t offsetStart = 0;

  for (OffsetPair& pair : ranges) {
    uint32_t offsetEnd = std::get<0>(pair);
    uintptr_t rangeSize = uintptr_t(offsetEnd - offsetStart);
    const char* rangeName = std::get<1>(pair).get();

    CollectPerfSpewerJitCodeProfile(basePtr + offsetStart, rangeSize,
                                    rangeName);
    offsetStart = offsetEnd;
  }

  MOZ_ASSERT(offsetStart <= code->instructionsSize());
  ranges.clear();
}
