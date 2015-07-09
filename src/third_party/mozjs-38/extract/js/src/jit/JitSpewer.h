/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitSpewer_h
#define jit_JitSpewer_h

#include "mozilla/DebugOnly.h"

#include <stdarg.h>

#include "jit/C1Spewer.h"
#include "jit/JSONSpewer.h"
#include "js/RootingAPI.h"

namespace js {
namespace jit {

// New channels may be added below.
#define JITSPEW_CHANNEL_LIST(_)             \
    /* Information during escape analysis */\
    _(Escape)                               \
    /* Information during alias analysis */ \
    _(Alias)                                \
    /* Information during GVN */            \
    _(GVN)                                  \
    /* Information during sinking */        \
    _(Sink)                                 \
    /* Information during Range analysis */ \
    _(Range)                                \
    /* Information during loop unrolling */ \
    _(Unrolling)                            \
    /* Information during LICM */           \
    _(LICM)                                 \
    /* Information during regalloc */       \
    _(RegAlloc)                             \
    /* Information during inlining */       \
    _(Inlining)                             \
    /* Information during codegen */        \
    _(Codegen)                              \
    /* Debug info about safepoints */       \
    _(Safepoints)                           \
    /* Debug info about Pools*/             \
    _(Pools)                                \
    /* Profiling-related information */     \
    _(Profiling)                            \
    /* Information of tracked opt strats */ \
    _(OptimizationTracking)                 \
    /* Debug info about the I$ */           \
    _(CacheFlush)                           \
                                            \
    /* BASELINE COMPILER SPEW */            \
                                            \
    /* Aborting Script Compilation. */      \
    _(BaselineAbort)                        \
    /* Script Compilation. */               \
    _(BaselineScripts)                      \
    /* Detailed op-specific spew. */        \
    _(BaselineOp)                           \
    /* Inline caches. */                    \
    _(BaselineIC)                           \
    /* Inline cache fallbacks. */           \
    _(BaselineICFallback)                   \
    /* OSR from Baseline => Ion. */         \
    _(BaselineOSR)                          \
    /* Bailouts. */                         \
    _(BaselineBailouts)                     \
    /* Debug Mode On Stack Recompile . */   \
    _(BaselineDebugModeOSR)                 \
                                            \
    /* ION COMPILER SPEW */                 \
                                            \
    /* Used to abort SSA construction */    \
    _(IonAbort)                             \
    /* Information about compiled scripts */\
    _(IonScripts)                           \
    /* Info about failing to log script */  \
    _(IonLogs)                              \
    /* Information during MIR building */   \
    _(IonMIR)                               \
    /* Information during bailouts */       \
    _(IonBailouts)                          \
    /* Information during OSI */            \
    _(IonInvalidate)                        \
    /* Debug info about snapshots */        \
    _(IonSnapshots)                         \
    /* Generated inline cache stubs */      \
    _(IonIC)

enum JitSpewChannel {
#define JITSPEW_CHANNEL(name) JitSpew_##name,
    JITSPEW_CHANNEL_LIST(JITSPEW_CHANNEL)
#undef JITSPEW_CHANNEL
    JitSpew_Terminator
};


// The JitSpewer is only available on debug builds.
// None of the global functions have effect on non-debug builds.
static const int NULL_ID = -1;

#ifdef DEBUG

class IonSpewer
{
  private:
    MIRGraph* graph;
    C1Spewer c1Spewer;
    JSONSpewer jsonSpewer;
    bool inited_;

  public:
    IonSpewer()
      : graph(nullptr), inited_(false)
    { }

    // File output is terminated safely upon destruction.
    ~IonSpewer();

    bool init();
    void beginFunction(MIRGraph* graph, JS::HandleScript);
    bool isSpewingFunction() const;
    void spewPass(const char* pass);
    void spewPass(const char* pass, LinearScanAllocator* ra);
    void endFunction();
};

class IonSpewFunction
{
  public:
    IonSpewFunction(MIRGraph* graph, JS::HandleScript function);
    ~IonSpewFunction();
};

void IonSpewNewFunction(MIRGraph* graph, JS::HandleScript function);
void IonSpewPass(const char* pass);
void IonSpewPass(const char* pass, LinearScanAllocator* ra);
void IonSpewEndFunction();

void CheckLogging();
extern FILE* JitSpewFile;
void JitSpew(JitSpewChannel channel, const char* fmt, ...);
void JitSpewStart(JitSpewChannel channel, const char* fmt, ...);
void JitSpewCont(JitSpewChannel channel, const char* fmt, ...);
void JitSpewFin(JitSpewChannel channel);
void JitSpewHeader(JitSpewChannel channel);
bool JitSpewEnabled(JitSpewChannel channel);
void JitSpewVA(JitSpewChannel channel, const char* fmt, va_list ap);
void JitSpewStartVA(JitSpewChannel channel, const char* fmt, va_list ap);
void JitSpewContVA(JitSpewChannel channel, const char* fmt, va_list ap);
void JitSpewDef(JitSpewChannel channel, const char* str, MDefinition* def);

void EnableChannel(JitSpewChannel channel);
void DisableChannel(JitSpewChannel channel);
void EnableIonDebugLogging();

#else

static inline void IonSpewNewFunction(MIRGraph* graph, JS::HandleScript function)
{ }
static inline void IonSpewPass(const char* pass)
{ }
static inline void IonSpewPass(const char* pass, LinearScanAllocator* ra)
{ }
static inline void IonSpewEndFunction()
{ }

static inline void CheckLogging()
{ }
static FILE* const JitSpewFile = nullptr;
static inline void JitSpew(JitSpewChannel, const char* fmt, ...)
{ }
static inline void JitSpewStart(JitSpewChannel channel, const char* fmt, ...)
{ }
static inline void JitSpewCont(JitSpewChannel channel, const char* fmt, ...)
{ }
static inline void JitSpewFin(JitSpewChannel channel)
{ }

static inline void JitSpewHeader(JitSpewChannel channel)
{ }
static inline bool JitSpewEnabled(JitSpewChannel channel)
{ return false; }
static inline void JitSpewVA(JitSpewChannel channel, const char* fmt, va_list ap)
{ }
static inline void JitSpewDef(JitSpewChannel channel, const char* str, MDefinition* def)
{ }

static inline void EnableChannel(JitSpewChannel)
{ }
static inline void DisableChannel(JitSpewChannel)
{ }
static inline void EnableIonDebugLogging()
{ }

#endif /* DEBUG */

template <JitSpewChannel Channel>
class AutoDisableSpew
{
    mozilla::DebugOnly<bool> enabled_;

  public:
    AutoDisableSpew()
      : enabled_(JitSpewEnabled(Channel))
    {
        DisableChannel(Channel);
    }

    ~AutoDisableSpew()
    {
#ifdef DEBUG
        if (enabled_)
            EnableChannel(Channel);
#endif
    }
};

} /* ion */
} /* js */

#endif /* jit_JitSpewer_h */
