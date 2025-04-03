/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitSpewer_h
#define jit_JitSpewer_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerPrintfMacros.h"

#include <stdarg.h>

#include "jit/JSONSpewer.h"
#include "js/Printer.h"
#include "js/TypeDecls.h"

enum JSValueType : uint8_t;

namespace js {
namespace jit {

// New channels may be added below.
#define JITSPEW_CHANNEL_LIST(_)            \
  /* Information during sinking */         \
  _(Prune)                                 \
  /* Information during escape analysis */ \
  _(Escape)                                \
  /* Information during alias analysis */  \
  _(Alias)                                 \
  /* Information during alias analysis */  \
  _(AliasSummaries)                        \
  /* Information during GVN */             \
  _(GVN)                                   \
  /* Information during sinking */         \
  _(Sink)                                  \
  /* Information during Range analysis */  \
  _(Range)                                 \
  /* Information during LICM */            \
  _(LICM)                                  \
  /* Info about fold linear constants */   \
  _(FLAC)                                  \
  /* Effective address analysis info */    \
  _(EAA)                                   \
  /* Wasm Bounds Check Elimination */      \
  _(WasmBCE)                               \
  /* Information during regalloc */        \
  _(RegAlloc)                              \
  /* Information during inlining */        \
  _(Inlining)                              \
  /* Information during codegen */         \
  _(Codegen)                               \
  /* Debug info about safepoints */        \
  _(Safepoints)                            \
  /* Debug info about Pools*/              \
  _(Pools)                                 \
  /* Profiling-related information */      \
  _(Profiling)                             \
  /* Debug info about the I$ */            \
  _(CacheFlush)                            \
  /* Info about redundant shape guards */  \
  _(RedundantShapeGuards)                  \
  /* Info about redundant GC barriers */   \
  _(RedundantGCBarriers)                   \
  /* Output a list of MIR expressions */   \
  _(MIRExpressions)                        \
  /* Spew Tracelogger summary stats */     \
  _(ScriptStats)                           \
                                           \
  /* BASELINE COMPILER SPEW */             \
                                           \
  /* Aborting Script Compilation. */       \
  _(BaselineAbort)                         \
  /* Script Compilation. */                \
  _(BaselineScripts)                       \
  /* Detailed op-specific spew. */         \
  _(BaselineOp)                            \
  /* Inline caches. */                     \
  _(BaselineIC)                            \
  /* Inline cache fallbacks. */            \
  _(BaselineICFallback)                    \
  /* OSR from Baseline => Ion. */          \
  _(BaselineOSR)                           \
  /* Bailouts. */                          \
  _(BaselineBailouts)                      \
  /* Debug Mode On Stack Recompile . */    \
  _(BaselineDebugModeOSR)                  \
                                           \
  /* ION COMPILER SPEW */                  \
                                           \
  /* Used to abort SSA construction */     \
  _(IonAbort)                              \
  /* Information about compiled scripts */ \
  _(IonScripts)                            \
  /* Info about failing to log script */   \
  _(IonSyncLogs)                           \
  /* Information during MIR building */    \
  _(IonMIR)                                \
  /* Information during bailouts */        \
  _(IonBailouts)                           \
  /* Information during OSI */             \
  _(IonInvalidate)                         \
  /* Debug info about snapshots */         \
  _(IonSnapshots)                          \
  /* Generated inline cache stubs */       \
  _(IonIC)                                 \
                                           \
  /* WARP SPEW */                          \
                                           \
  /* Generated WarpSnapshots */            \
  _(WarpSnapshots)                         \
  /* CacheIR transpiler logging */         \
  _(WarpTranspiler)                        \
  /* Trial inlining for Warp */            \
  _(WarpTrialInlining)

enum JitSpewChannel {
#define JITSPEW_CHANNEL(name) JitSpew_##name,
  JITSPEW_CHANNEL_LIST(JITSPEW_CHANNEL)
#undef JITSPEW_CHANNEL
      JitSpew_Terminator
};

class BacktrackingAllocator;
class MDefinition;
class MIRGenerator;
class MIRGraph;
class TempAllocator;

// The JitSpewer is only available on debug builds.
// None of the global functions have effect on non-debug builds.
#ifdef JS_JITSPEW

// Class made to hold the MIR and LIR graphs of an Wasm / Ion compilation.
class GraphSpewer {
 private:
  MIRGraph* graph_;
  LSprinter jsonPrinter_;
  JSONSpewer jsonSpewer_;

 public:
  explicit GraphSpewer(TempAllocator* alloc);

  bool isSpewing() const { return graph_; }
  void init(MIRGraph* graph, JSScript* function);
  void beginFunction(JSScript* function);
  void beginWasmFunction(unsigned funcIndex);
  void spewPass(const char* pass);
  void spewPass(const char* pass, BacktrackingAllocator* ra);
  void endFunction();

  void dump(Fprinter& json);
};

void SpewBeginFunction(MIRGenerator* mir, JSScript* function);
void SpewBeginWasmFunction(MIRGenerator* mir, unsigned funcIndex);

class AutoSpewEndFunction {
 private:
  MIRGenerator* mir_;

 public:
  explicit AutoSpewEndFunction(MIRGenerator* mir) : mir_(mir) {}
  ~AutoSpewEndFunction();
};

void CheckLogging();
Fprinter& JitSpewPrinter();

class JitSpewIndent {
  JitSpewChannel channel_;

 public:
  explicit JitSpewIndent(JitSpewChannel channel);
  ~JitSpewIndent();
};

void JitSpew(JitSpewChannel channel, const char* fmt, ...)
    MOZ_FORMAT_PRINTF(2, 3);
void JitSpewStart(JitSpewChannel channel, const char* fmt, ...)
    MOZ_FORMAT_PRINTF(2, 3);
void JitSpewCont(JitSpewChannel channel, const char* fmt, ...)
    MOZ_FORMAT_PRINTF(2, 3);
void JitSpewFin(JitSpewChannel channel);
void JitSpewHeader(JitSpewChannel channel);
bool JitSpewEnabled(JitSpewChannel channel);
void JitSpewVA(JitSpewChannel channel, const char* fmt, va_list ap)
    MOZ_FORMAT_PRINTF(2, 0);
void JitSpewStartVA(JitSpewChannel channel, const char* fmt, va_list ap)
    MOZ_FORMAT_PRINTF(2, 0);
void JitSpewContVA(JitSpewChannel channel, const char* fmt, va_list ap)
    MOZ_FORMAT_PRINTF(2, 0);
void JitSpewDef(JitSpewChannel channel, const char* str, MDefinition* def);

void EnableChannel(JitSpewChannel channel);
void DisableChannel(JitSpewChannel channel);
void EnableIonDebugSyncLogging();
void EnableIonDebugAsyncLogging();

const char* ValTypeToString(JSValueType type);

#  define JitSpewIfEnabled(channel, fmt, ...) \
    do {                                      \
      if (JitSpewEnabled(channel)) {          \
        JitSpew(channel, fmt, __VA_ARGS__);   \
      }                                       \
    } while (false);

#else

class GraphSpewer {
 public:
  explicit GraphSpewer(TempAllocator* alloc) {}

  bool isSpewing() { return false; }
  void init(MIRGraph* graph, JSScript* function) {}
  void beginFunction(JSScript* function) {}
  void spewPass(const char* pass) {}
  void spewPass(const char* pass, BacktrackingAllocator* ra) {}
  void endFunction() {}

  void dump(Fprinter& c1, Fprinter& json) {}
};

static inline void SpewBeginFunction(MIRGenerator* mir, JSScript* function) {}
static inline void SpewBeginWasmFunction(MIRGenerator* mir,
                                         unsigned funcIndex) {}

class AutoSpewEndFunction {
 public:
  explicit AutoSpewEndFunction(MIRGenerator* mir) {}
  ~AutoSpewEndFunction() {}
};

static inline void CheckLogging() {}
static inline Fprinter& JitSpewPrinter() {
  MOZ_CRASH("No empty backend for JitSpewPrinter");
}

class JitSpewIndent {
 public:
  explicit JitSpewIndent(JitSpewChannel channel) {}
  ~JitSpewIndent() {}
};

// The computation of some of the argument of the spewing functions might be
// costly, thus we use variaidic macros to ignore any argument of these
// functions.
static inline void JitSpewCheckArguments(JitSpewChannel channel,
                                         const char* fmt) {}

#  define JitSpewCheckExpandedArgs(channel, fmt, ...) \
    JitSpewCheckArguments(channel, fmt)
#  define JitSpewCheckExpandedArgs_(ArgList) \
    JitSpewCheckExpandedArgs ArgList /* Fix MSVC issue */
#  define JitSpew(...) JitSpewCheckExpandedArgs_((__VA_ARGS__))
#  define JitSpewStart(...) JitSpewCheckExpandedArgs_((__VA_ARGS__))
#  define JitSpewCont(...) JitSpewCheckExpandedArgs_((__VA_ARGS__))

#  define JitSpewIfEnabled(channel, fmt, ...) \
    JitSpewCheckArguments(channel, fmt)

static inline void JitSpewFin(JitSpewChannel channel) {}

static inline void JitSpewHeader(JitSpewChannel channel) {}
static inline bool JitSpewEnabled(JitSpewChannel channel) { return false; }
static inline MOZ_FORMAT_PRINTF(2, 0) void JitSpewVA(JitSpewChannel channel,
                                                     const char* fmt,
                                                     va_list ap) {}
static inline void JitSpewDef(JitSpewChannel channel, const char* str,
                              MDefinition* def) {}

static inline void EnableChannel(JitSpewChannel) {}
static inline void DisableChannel(JitSpewChannel) {}
static inline void EnableIonDebugSyncLogging() {}
static inline void EnableIonDebugAsyncLogging() {}

#endif /* JS_JITSPEW */

}  // namespace jit
}  // namespace js

#endif /* jit_JitSpewer_h */
