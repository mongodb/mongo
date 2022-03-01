/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CacheIRHealth_h
#define jit_CacheIRHealth_h

#ifdef JS_CACHEIR_SPEW

#  include "mozilla/Sprintf.h"

#  include "jit/CacheIR.h"

namespace js {
namespace jit {

class ICEntry;
class ICStub;
class ICCacheIRStub;
class ICFallbackStub;

// [SMDOC] CacheIR Health Report
//
// The goal of CacheIR health report is to make the costlier
// CacheIR stubs more apparent and easier to diagnose.
// This is done by using the scores assigned to different CacheIROps in
// CacheIROps.yaml (see the description of cost_estimate in the
// aforementioned file for how these scores are determined), summing
// the score of each op generated for a particular stub together, and displaying
// this score for each stub in an inline cache. The higher the total stub score,
// the more expensive the stub is.
//
// There are a few ways to generate a health report for a script:
// 1. Simply running a JS program with the evironment variable
//    SPEW=CacheIRHealthReport. We generate a health report for a script
//    whenever we reach the trial inlining threshold.
//      ex) SPEW=CacheIRHealthReport dist/bin/js jsprogram.js
// 2. In the shell you can call cacheIRHealthReport() with no arguments and a
// report
//    will be generated for all scripts in the current zone.
//      ex) cacheIRHealthReport()
// 3. You may also call cacheIRHealthReport() on a particular function to see
// the
//    health report associated with that function's script.
//      ex) cacheIRHealthReport(foo)
//
// Once you have generated a health report, you may go to
// https://carolinecullen.github.io/cacheirhealthreport/ to visualize the data
// and aid in understanding what may be going wrong with the CacheIR for a
// particular stub. For more information about the tool and why a particular
// script, inline cache entry, or stub is unhappy go to:
// https://carolinecullen.github.io/cacheirhealthreport/info.html
//
enum SpewContext : uint8_t { Shell, Transition, TrialInlining };

class CacheIRHealth {
  enum Happiness : uint8_t { Sad, MediumSad, MediumHappy, Happy };

  // Get happiness from health score.
  Happiness determineStubHappiness(uint32_t stubHealthScore);
  // Health of an individual stub.
  Happiness spewStubHealth(AutoStructuredSpewer& spew, ICCacheIRStub* stub);
  // If there is more than just a fallback stub in an IC Entry, then additional
  // information about the IC entry.
  bool spewNonFallbackICInformation(AutoStructuredSpewer& spew,
                                    ICStub* firstStub,
                                    Happiness* entryHappiness);
  // Health of all the stubs in an individual CacheIR Entry.
  bool spewICEntryHealth(AutoStructuredSpewer& spew, HandleScript script,
                         ICEntry* entry, ICFallbackStub* fallback,
                         jsbytecode* pc, JSOp op, Happiness* entryHappiness);

 public:
  // Spews the final hit count for scripts where we care about its final hit
  // count.
  void spewScriptFinalWarmUpCount(JSContext* cx, const char* filename,
                                  JSScript* script, uint32_t warmUpCount);
  // Spew the health of a particular ICEntry only.
  void healthReportForIC(JSContext* cx, ICEntry* entry,
                         ICFallbackStub* fallback, HandleScript script,
                         SpewContext context);
  // If a JitScript exists, spew the health of all ICEntries that exist
  // for the specified script.
  void healthReportForScript(JSContext* cx, HandleScript script,
                             SpewContext context);
};

}  // namespace jit
}  // namespace js

#endif /* JS_CACHEIR_SPEW */
#endif /* jit_CacheIRHealth_h */
