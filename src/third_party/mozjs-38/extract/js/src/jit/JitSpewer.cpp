/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef DEBUG

#include "jit/JitSpewer.h"

#include "jit/Ion.h"
#include "jit/MIR.h"

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

// IonSpewer singleton.
static IonSpewer ionspewer;

static bool LoggingChecked = false;
static uint32_t LoggingBits = 0;
static uint32_t filteredOutCompilations = 0;

static const char * const ChannelNames[] =
{
#define JITSPEW_CHANNEL(name) #name,
    JITSPEW_CHANNEL_LIST(JITSPEW_CHANNEL)
#undef JITSPEW_CHANNEL
};

static bool
FilterContainsLocation(HandleScript function)
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
jit::EnableIonDebugLogging()
{
    EnableChannel(JitSpew_IonLogs);
    ionspewer.init();
}

void
jit::IonSpewNewFunction(MIRGraph* graph, HandleScript func)
{
    if (GetJitContext()->runtime->onMainThread())
        ionspewer.beginFunction(graph, func);
}

void
jit::IonSpewPass(const char* pass)
{
    if (GetJitContext()->runtime->onMainThread())
        ionspewer.spewPass(pass);
}

void
jit::IonSpewPass(const char* pass, LinearScanAllocator* ra)
{
    if (GetJitContext()->runtime->onMainThread())
        ionspewer.spewPass(pass, ra);
}

void
jit::IonSpewEndFunction()
{
    if (GetJitContext()->runtime->onMainThread())
        ionspewer.endFunction();
}


IonSpewer::~IonSpewer()
{
    if (!inited_)
        return;

    c1Spewer.finish();
    jsonSpewer.finish();
}

bool
IonSpewer::init()
{
    if (inited_)
        return true;

    if (!c1Spewer.init(JIT_SPEW_DIR "ion.cfg"))
        return false;
    if (!jsonSpewer.init(JIT_SPEW_DIR "ion.json"))
        return false;

    inited_ = true;
    return true;
}

bool
IonSpewer::isSpewingFunction() const
{
    return inited_ && graph;
}

void
IonSpewer::beginFunction(MIRGraph* graph, HandleScript function)
{
    if (!inited_)
        return;

    if (!FilterContainsLocation(function)) {
        MOZ_ASSERT(!this->graph);
        // filter out logs during the compilation.
        filteredOutCompilations++;
        return;
    }

    this->graph = graph;

    c1Spewer.beginFunction(graph, function);
    jsonSpewer.beginFunction(function);
}

void
IonSpewer::spewPass(const char* pass)
{
    if (!isSpewingFunction())
        return;

    c1Spewer.spewPass(pass);
    jsonSpewer.beginPass(pass);
    jsonSpewer.spewMIR(graph);
    jsonSpewer.spewLIR(graph);
    jsonSpewer.endPass();
}

void
IonSpewer::spewPass(const char* pass, LinearScanAllocator* ra)
{
    if (!isSpewingFunction())
        return;

    c1Spewer.spewPass(pass);
    c1Spewer.spewIntervals(pass, ra);
    jsonSpewer.beginPass(pass);
    jsonSpewer.spewMIR(graph);
    jsonSpewer.spewLIR(graph);
    jsonSpewer.spewIntervals(ra);
    jsonSpewer.endPass();
}

void
IonSpewer::endFunction()
{
    if (!isSpewingFunction()) {
        if (inited_) {
            MOZ_ASSERT(filteredOutCompilations != 0);
            filteredOutCompilations--;
        }
        return;
    }

    c1Spewer.endFunction();
    jsonSpewer.endFunction();

    this->graph = nullptr;
}


FILE* jit::JitSpewFile = nullptr;

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
            "  escape     Escape analysis\n"
            "  alias      Alias analysis\n"
            "  gvn        Global Value Numbering\n"
            "  licm       Loop invariant code motion\n"
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
        EnableIonDebugLogging();
    if (ContainsFlag(env, "profiling"))
        EnableChannel(JitSpew_Profiling);
    if (ContainsFlag(env, "trackopts"))
        EnableChannel(JitSpew_OptimizationTracking);
    if (ContainsFlag(env, "all"))
        LoggingBits = uint32_t(-1);

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

    JitSpewFile = stderr;
}

void
jit::JitSpewStartVA(JitSpewChannel channel, const char* fmt, va_list ap)
{
    if (!JitSpewEnabled(channel))
        return;

    JitSpewHeader(channel);
    vfprintf(stderr, fmt, ap);
}

void
jit::JitSpewContVA(JitSpewChannel channel, const char* fmt, va_list ap)
{
    if (!JitSpewEnabled(channel))
        return;

    vfprintf(stderr, fmt, ap);
}

void
jit::JitSpewFin(JitSpewChannel channel)
{
    if (!JitSpewEnabled(channel))
        return;

    fprintf(stderr, "\n");
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
    fprintf(JitSpewFile, "%s", str);
    def->dump(JitSpewFile);
    def->dumpLocation(JitSpewFile);
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

    fprintf(stderr, "[%s] ", ChannelNames[channel]);
}

bool
jit::JitSpewEnabled(JitSpewChannel channel)
{
    MOZ_ASSERT(LoggingChecked);
    return (LoggingBits & (1 << uint32_t(channel))) && !filteredOutCompilations;
}

void
jit::EnableChannel(JitSpewChannel channel)
{
    MOZ_ASSERT(LoggingChecked);
    LoggingBits |= (1 << uint32_t(channel));
}

void
jit::DisableChannel(JitSpewChannel channel)
{
    MOZ_ASSERT(LoggingChecked);
    LoggingBits &= ~(1 << uint32_t(channel));
}

IonSpewFunction::IonSpewFunction(MIRGraph* graph, JS::HandleScript function)
{
    IonSpewNewFunction(graph, function);
}

IonSpewFunction::~IonSpewFunction()
{
    IonSpewEndFunction();
}

#endif /* DEBUG */

