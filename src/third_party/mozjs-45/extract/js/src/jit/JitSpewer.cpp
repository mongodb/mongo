/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef JS_JITSPEW

#include "jit/JitSpewer.h"

#include "mozilla/Atomics.h"

#include "jit/Ion.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"

#include "vm/HelperThreads.h"

#ifndef JIT_SPEW_DIR
# if defined(_WIN32)
#  define JIT_SPEW_DIR ""
# elif defined(__ANDROID__)
#  define JIT_SPEW_DIR "/data/local/tmp/"
# else
#  define JIT_SPEW_DIR "/tmp/"
# endif
#endif

using namespace js;
using namespace js::jit;

class IonSpewer
{
  private:
    PRLock* outputLock_;
    Fprinter c1Output_;
    Fprinter jsonOutput_;
    bool firstFunction_;
    bool asyncLogging_;
    bool inited_;

    void release();

  public:
    IonSpewer()
      : firstFunction_(false),
        asyncLogging_(false),
        inited_(false)
    { }

    // File output is terminated safely upon destruction.
    ~IonSpewer();

    bool init();
    bool isEnabled() {
        return inited_;
    }
    void setAsyncLogging(bool incremental) {
        asyncLogging_ = incremental;
    }
    bool getAsyncLogging() {
        return asyncLogging_;
    }

    void beginFunction();
    void spewPass(GraphSpewer* gs);
    void endFunction(GraphSpewer* gs);

    // Lock used to sequentialized asynchronous compilation output.
    void lockOutput() {
        PR_Lock(outputLock_);
    }
    void unlockOutput() {
        PR_Unlock(outputLock_);
    }
};

class MOZ_RAII AutoLockIonSpewerOutput
{
  private:
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
  public:
    explicit AutoLockIonSpewerOutput(MOZ_GUARD_OBJECT_NOTIFIER_ONLY_PARAM);
    ~AutoLockIonSpewerOutput();
};

// IonSpewer singleton.
static IonSpewer ionspewer;

static bool LoggingChecked = false;
static_assert(JitSpew_Terminator <= 64, "Increase the size of the LoggingBits global.");
static uint64_t LoggingBits = 0;
static mozilla::Atomic<uint32_t, mozilla::Relaxed> filteredOutCompilations(0);

static const char * const ChannelNames[] =
{
#define JITSPEW_CHANNEL(name) #name,
    JITSPEW_CHANNEL_LIST(JITSPEW_CHANNEL)
#undef JITSPEW_CHANNEL
};

static size_t ChannelIndentLevel[] =
{
#define JITSPEW_CHANNEL(name) 0,
    JITSPEW_CHANNEL_LIST(JITSPEW_CHANNEL)
#undef JITSPEW_CHANNEL
};

static bool
FilterContainsLocation(JSScript* function)
{
    static const char* filter = getenv("IONFILTER");

    // If there is no filter we accept all outputs.
    if (!filter || !filter[0])
        return true;

    // Disable asm.js output when filter is set.
    if (!function)
        return false;

    const char* filename = function->filename();
    const size_t line = function->lineno();
    const size_t filelen = strlen(filename);
    const char* index = strstr(filter, filename);
    while (index) {
        if (index == filter || index[-1] == ',') {
            if (index[filelen] == 0 || index[filelen] == ',')
                return true;
            if (index[filelen] == ':' && line != size_t(-1)) {
                size_t read_line = strtoul(&index[filelen + 1], nullptr, 10);
                if (read_line == line)
                    return true;
            }
        }
        index = strstr(index + filelen, filename);
    }
    return false;
}

void
jit::EnableIonDebugSyncLogging()
{
    ionspewer.init();
    ionspewer.setAsyncLogging(false);
    EnableChannel(JitSpew_IonSyncLogs);
}

void
jit::EnableIonDebugAsyncLogging()
{
    ionspewer.init();
    ionspewer.setAsyncLogging(true);
}

void
IonSpewer::release()
{
    if (c1Output_.isInitialized())
        c1Output_.finish();
    if (jsonOutput_.isInitialized())
        jsonOutput_.finish();
    if (outputLock_)
        PR_DestroyLock(outputLock_);
    outputLock_ = nullptr;
    inited_ = false;
}

bool
IonSpewer::init()
{
    if (inited_)
        return true;

    outputLock_ = PR_NewLock();
    if (!outputLock_ ||
        !c1Output_.init(JIT_SPEW_DIR "ion.cfg") ||
        !jsonOutput_.init(JIT_SPEW_DIR "ion.json"))
    {
        release();
        return false;
    }

    jsonOutput_.printf("{\n  \"functions\": [\n");
    firstFunction_ = true;

    inited_ = true;
    return true;
}

AutoLockIonSpewerOutput::AutoLockIonSpewerOutput(MOZ_GUARD_OBJECT_NOTIFIER_ONLY_PARAM_IN_IMPL)
{
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    ionspewer.lockOutput();
}

AutoLockIonSpewerOutput::~AutoLockIonSpewerOutput()
{
    ionspewer.unlockOutput();
}

void
IonSpewer::beginFunction()
{
    // If we are doing a synchronous logging then we spew everything as we go,
    // as this is useful in case of failure during the compilation. On the other
    // hand, it is recommended to disabled off main thread compilation.
    if (!getAsyncLogging() && !firstFunction_) {
        AutoLockIonSpewerOutput outputLock;
        jsonOutput_.put(","); // separate functions
    }
}

void
IonSpewer::spewPass(GraphSpewer* gs)
{
    if (!getAsyncLogging()) {
        AutoLockIonSpewerOutput outputLock;
        gs->dump(c1Output_, jsonOutput_);
    }
}

void
IonSpewer::endFunction(GraphSpewer* gs)
{
    AutoLockIonSpewerOutput outputLock;
    if (getAsyncLogging() && !firstFunction_)
        jsonOutput_.put(","); // separate functions

    gs->dump(c1Output_, jsonOutput_);
    firstFunction_ = false;
}

IonSpewer::~IonSpewer()
{
    if (!inited_)
        return;

    jsonOutput_.printf("\n]}\n");
    release();
}

GraphSpewer::GraphSpewer(TempAllocator *alloc)
  : graph_(nullptr),
    c1Printer_(alloc->lifoAlloc()),
    jsonPrinter_(alloc->lifoAlloc()),
    c1Spewer_(c1Printer_),
    jsonSpewer_(jsonPrinter_)
{
}

void
GraphSpewer::init(MIRGraph* graph, JSScript* function)
{
    MOZ_ASSERT(!isSpewing());
    if (!ionspewer.isEnabled())
        return;

    if (!FilterContainsLocation(function)) {
        // filter out logs during the compilation.
        filteredOutCompilations++;
        MOZ_ASSERT(!isSpewing());
        return;
    }

    graph_ = graph;
    MOZ_ASSERT(isSpewing());
}

void
GraphSpewer::beginFunction(JSScript* function)
{
    if (!isSpewing())
        return;

    c1Spewer_.beginFunction(graph_, function);
    jsonSpewer_.beginFunction(function);

    ionspewer.beginFunction();
}

void
GraphSpewer::spewPass(const char* pass)
{
    if (!isSpewing())
        return;

    c1Spewer_.spewPass(pass);

    jsonSpewer_.beginPass(pass);
    jsonSpewer_.spewMIR(graph_);
    jsonSpewer_.spewLIR(graph_);
    jsonSpewer_.endPass();

    ionspewer.spewPass(this);
}

void
GraphSpewer::spewPass(const char* pass, BacktrackingAllocator* ra)
{
    if (!isSpewing())
        return;

    c1Spewer_.spewPass(pass);
    c1Spewer_.spewRanges(pass, ra);

    jsonSpewer_.beginPass(pass);
    jsonSpewer_.spewMIR(graph_);
    jsonSpewer_.spewLIR(graph_);
    jsonSpewer_.spewRanges(ra);
    jsonSpewer_.endPass();

    ionspewer.spewPass(this);
}

void
GraphSpewer::endFunction()
{
    if (!ionspewer.isEnabled())
        return;

    if (!isSpewing()) {
        MOZ_ASSERT(filteredOutCompilations != 0);
        filteredOutCompilations--;
        return;
    }

    c1Spewer_.endFunction();
    jsonSpewer_.endFunction();

    ionspewer.endFunction(this);
    graph_ = nullptr;
}

void
GraphSpewer::dump(Fprinter& c1Out, Fprinter& jsonOut)
{
    if (!c1Printer_.hadOutOfMemory()) {
        c1Printer_.exportInto(c1Out);
        c1Out.flush();
    }
    c1Printer_.clear();

    if (!jsonPrinter_.hadOutOfMemory())
        jsonPrinter_.exportInto(jsonOut);
    else
        jsonOut.put("{}");
    jsonOut.flush();
    jsonPrinter_.clear();
}

void
jit::SpewBeginFunction(MIRGenerator* mir, JSScript* function)
{
    MIRGraph* graph = &mir->graph();
    mir->graphSpewer().init(graph, function);
    mir->graphSpewer().beginFunction(function);
}

AutoSpewEndFunction::~AutoSpewEndFunction()
{
    mir_->graphSpewer().endFunction();
}

Fprinter&
jit::JitSpewPrinter()
{
    static Fprinter out;
    return out;
}


static bool
ContainsFlag(const char* str, const char* flag)
{
    size_t flaglen = strlen(flag);
    const char* index = strstr(str, flag);
    while (index) {
        if ((index == str || index[-1] == ',') && (index[flaglen] == 0 || index[flaglen] == ','))
            return true;
        index = strstr(index + flaglen, flag);
    }
    return false;
}

void
jit::CheckLogging()
{
    if (LoggingChecked)
        return;
    LoggingChecked = true;
    const char* env = getenv("IONFLAGS");
    if (!env)
        return;
    if (strstr(env, "help")) {
        fflush(nullptr);
        printf(
            "\n"
            "usage: IONFLAGS=option,option,option,... where options can be:\n"
            "\n"
            "  aborts     Compilation abort messages\n"
            "  scripts    Compiled scripts\n"
            "  mir        MIR information\n"
            "  prune      Prune unused branches\n"
            "  escape     Escape analysis\n"
            "  alias      Alias analysis\n"
            "  gvn        Global Value Numbering\n"
            "  licm       Loop invariant code motion\n"
            "  sincos     Replace sin/cos by sincos\n"
            "  sink       Sink transformation\n"
            "  regalloc   Register allocation\n"
            "  inline     Inlining\n"
            "  snapshots  Snapshot information\n"
            "  codegen    Native code generation\n"
            "  bailouts   Bailouts\n"
            "  caches     Inline caches\n"
            "  osi        Invalidation\n"
            "  safepoints Safepoints\n"
            "  pools      Literal Pools (ARM only for now)\n"
            "  cacheflush Instruction Cache flushes (ARM only for now)\n"
            "  range      Range Analysis\n"
            "  unroll     Loop unrolling\n"
            "  logs       C1 and JSON visualization logging\n"
            "  logs-sync  Same as logs, but flushes between each pass (sync. compiled functions only).\n"
            "  profiling  Profiling-related information\n"
            "  trackopts  Optimization tracking information\n"
            "  all        Everything\n"
            "\n"
            "  bl-aborts  Baseline compiler abort messages\n"
            "  bl-scripts Baseline script-compilation\n"
            "  bl-op      Baseline compiler detailed op-specific messages\n"
            "  bl-ic      Baseline inline-cache messages\n"
            "  bl-ic-fb   Baseline IC fallback stub messages\n"
            "  bl-osr     Baseline IC OSR messages\n"
            "  bl-bails   Baseline bailouts\n"
            "  bl-dbg-osr Baseline debug mode on stack recompile messages\n"
            "  bl-all     All baseline spew\n"
            "\n"
        );
        exit(0);
        /*NOTREACHED*/
    }
    if (ContainsFlag(env, "aborts"))
        EnableChannel(JitSpew_IonAbort);
    if (ContainsFlag(env, "prune"))
        EnableChannel(JitSpew_Prune);
    if (ContainsFlag(env, "escape"))
        EnableChannel(JitSpew_Escape);
    if (ContainsFlag(env, "alias"))
        EnableChannel(JitSpew_Alias);
    if (ContainsFlag(env, "scripts"))
        EnableChannel(JitSpew_IonScripts);
    if (ContainsFlag(env, "mir"))
        EnableChannel(JitSpew_IonMIR);
    if (ContainsFlag(env, "gvn"))
        EnableChannel(JitSpew_GVN);
    if (ContainsFlag(env, "range"))
        EnableChannel(JitSpew_Range);
    if (ContainsFlag(env, "unroll"))
        EnableChannel(JitSpew_Unrolling);
    if (ContainsFlag(env, "licm"))
        EnableChannel(JitSpew_LICM);
    if (ContainsFlag(env, "sincos"))
        EnableChannel(JitSpew_Sincos);
    if (ContainsFlag(env, "sink"))
        EnableChannel(JitSpew_Sink);
    if (ContainsFlag(env, "regalloc"))
        EnableChannel(JitSpew_RegAlloc);
    if (ContainsFlag(env, "inline"))
        EnableChannel(JitSpew_Inlining);
    if (ContainsFlag(env, "snapshots"))
        EnableChannel(JitSpew_IonSnapshots);
    if (ContainsFlag(env, "codegen"))
        EnableChannel(JitSpew_Codegen);
    if (ContainsFlag(env, "bailouts"))
        EnableChannel(JitSpew_IonBailouts);
    if (ContainsFlag(env, "osi"))
        EnableChannel(JitSpew_IonInvalidate);
    if (ContainsFlag(env, "caches"))
        EnableChannel(JitSpew_IonIC);
    if (ContainsFlag(env, "safepoints"))
        EnableChannel(JitSpew_Safepoints);
    if (ContainsFlag(env, "pools"))
        EnableChannel(JitSpew_Pools);
    if (ContainsFlag(env, "cacheflush"))
        EnableChannel(JitSpew_CacheFlush);
    if (ContainsFlag(env, "logs"))
        EnableIonDebugAsyncLogging();
    if (ContainsFlag(env, "logs-sync"))
        EnableIonDebugSyncLogging();
    if (ContainsFlag(env, "profiling"))
        EnableChannel(JitSpew_Profiling);
    if (ContainsFlag(env, "trackopts"))
        EnableChannel(JitSpew_OptimizationTracking);
    if (ContainsFlag(env, "all"))
        LoggingBits = uint64_t(-1);

    if (ContainsFlag(env, "bl-aborts"))
        EnableChannel(JitSpew_BaselineAbort);
    if (ContainsFlag(env, "bl-scripts"))
        EnableChannel(JitSpew_BaselineScripts);
    if (ContainsFlag(env, "bl-op"))
        EnableChannel(JitSpew_BaselineOp);
    if (ContainsFlag(env, "bl-ic"))
        EnableChannel(JitSpew_BaselineIC);
    if (ContainsFlag(env, "bl-ic-fb"))
        EnableChannel(JitSpew_BaselineICFallback);
    if (ContainsFlag(env, "bl-osr"))
        EnableChannel(JitSpew_BaselineOSR);
    if (ContainsFlag(env, "bl-bails"))
        EnableChannel(JitSpew_BaselineBailouts);
    if (ContainsFlag(env, "bl-dbg-osr"))
        EnableChannel(JitSpew_BaselineDebugModeOSR);
    if (ContainsFlag(env, "bl-all")) {
        EnableChannel(JitSpew_BaselineAbort);
        EnableChannel(JitSpew_BaselineScripts);
        EnableChannel(JitSpew_BaselineOp);
        EnableChannel(JitSpew_BaselineIC);
        EnableChannel(JitSpew_BaselineICFallback);
        EnableChannel(JitSpew_BaselineOSR);
        EnableChannel(JitSpew_BaselineBailouts);
        EnableChannel(JitSpew_BaselineDebugModeOSR);
    }

    JitSpewPrinter().init(stderr);
}

JitSpewIndent::JitSpewIndent(JitSpewChannel channel)
  : channel_(channel)
{
    ChannelIndentLevel[channel]++;
}

JitSpewIndent::~JitSpewIndent()
{
    ChannelIndentLevel[channel_]--;
}

void
jit::JitSpewStartVA(JitSpewChannel channel, const char* fmt, va_list ap)
{
    if (!JitSpewEnabled(channel))
        return;

    JitSpewHeader(channel);
    Fprinter& out = JitSpewPrinter();
    out.vprintf(fmt, ap);
}

void
jit::JitSpewContVA(JitSpewChannel channel, const char* fmt, va_list ap)
{
    if (!JitSpewEnabled(channel))
        return;

    Fprinter& out = JitSpewPrinter();
    out.vprintf(fmt, ap);
}

void
jit::JitSpewFin(JitSpewChannel channel)
{
    if (!JitSpewEnabled(channel))
        return;

    Fprinter& out = JitSpewPrinter();
    out.put("\n");
}

void
jit::JitSpewVA(JitSpewChannel channel, const char* fmt, va_list ap)
{
    JitSpewStartVA(channel, fmt, ap);
    JitSpewFin(channel);
}

void
jit::JitSpew(JitSpewChannel channel, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    JitSpewVA(channel, fmt, ap);
    va_end(ap);
}

void
jit::JitSpewDef(JitSpewChannel channel, const char* str, MDefinition* def)
{
    if (!JitSpewEnabled(channel))
        return;

    JitSpewHeader(channel);
    Fprinter& out = JitSpewPrinter();
    out.put(str);
    def->dump(out);
    def->dumpLocation(out);
}

void
jit::JitSpewStart(JitSpewChannel channel, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    JitSpewStartVA(channel, fmt, ap);
    va_end(ap);
}
void
jit::JitSpewCont(JitSpewChannel channel, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    JitSpewContVA(channel, fmt, ap);
    va_end(ap);
}

void
jit::JitSpewHeader(JitSpewChannel channel)
{
    if (!JitSpewEnabled(channel))
        return;

    Fprinter& out = JitSpewPrinter();
    out.printf("[%s] ", ChannelNames[channel]);
    for (size_t i = ChannelIndentLevel[channel]; i != 0; i--)
        out.put("  ");
}

bool
jit::JitSpewEnabled(JitSpewChannel channel)
{
    MOZ_ASSERT(LoggingChecked);
    return (LoggingBits & (uint64_t(1) << uint32_t(channel))) && !filteredOutCompilations;
}

void
jit::EnableChannel(JitSpewChannel channel)
{
    MOZ_ASSERT(LoggingChecked);
    LoggingBits |= uint64_t(1) << uint32_t(channel);
}

void
jit::DisableChannel(JitSpewChannel channel)
{
    MOZ_ASSERT(LoggingChecked);
    LoggingBits &= ~(uint64_t(1) << uint32_t(channel));
}

#endif /* JS_JITSPEW */

