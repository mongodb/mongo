/* -*- Mode: javascript; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This function can be used to "sanitize" a new global for fuzzing in such
// a way that permanent side-effects, hangs and behavior that could be harmful
// to libFuzzer targets is reduced to a minimum.
function sanitizeGlobal(g) {
  let lfFuncs = {
    // Noisy functions (output)
    backtrace: function () { },
    getBacktrace: function () { },
    help: function () { },
    print: function (s) { return s.toString(); },
    printErr: function (s) { return s.toString(); },
    putstr: function (s) { return s.toString(); },
    stackDump: function () { },
    dumpHeap: function () { },
    dumpScopeChain: function () { },
    dumpObjectWrappers: function () { },
    dumpGCArenaInfo: function () { },
    printProfilerEvents: function () { },

    // Harmful functions (hangs, timeouts, leaks)
    getLcovInfo: function () { },
    readline: function () { },
    readlineBuf: function () { },
    timeout: function () { },
    quit: function () { },
    interruptIf: function () { },
    terminate: function () { },
    invokeInterruptCallback: function () { },
    setInterruptCallback: function () { },
    intern: function () { },
    evalInWorker: function () { },
    sleep: function () { },
    cacheEntry: function () { },
    streamCacheEntry: function () { },
    createMappedArrayBuffer: function () { },
    wasmCompileInSeparateProcess: function () { },
    gcparam: function () { },
    newGlobal: function () { return g; },

    // Harmful functions (throw)
    assertEq: function (a, b) { return a.toString() == b.toString(); },
    throwError: function () { },
    reportOutOfMemory: function () { },
    throwOutOfMemory: function () { },
    reportLargeAllocationFailure: function () { },

    // Functions that need limiting
    gczeal: function (m, f) { return gczeal(m, 100); },
    startgc: function (n, o) { startgc(n > 20 ? 20 : n, o); },
    gcslice: function (n) { gcslice(n > 20 ? 20 : n); },

    // Global side-effects
    deterministicgc: function () { },
    fullcompartmentchecks: function () { },
    setIonCheckGraphCoherency: function () { },
    enableShellAllocationMetadataBuilder: function () { },
    setTimeResolution: function () { },
    options: function () { return "tracejit,methodjit,typeinfer"; },
    setJitCompilerOption: function () { },
    clearLastWarning: function () { },
    enableSingleStepProfiling: function () { },
    disableSingleStepProfiling: function () { },
    enableGeckoProfiling: function () { },
    enableGeckoProfilingWithSlowAssertions: function () { },
    disableGeckoProfiling: function () { },
    enqueueJob: function () { },
    globalOfFirstJobInQueue: function () { },
    drainJobQueue: function () { },
    setPromiseRejectionTrackerCallback: function () { },
    startTimingMutator: function () { },
    stopTimingMutator: function () { },
    setModuleLoadHook: function () { },
    // Left enabled, as it is required for now to avoid leaks
    //setModuleResolveHook: function() {},
    setModuleMetadataHook: function () { },
    setModuleDynamicImportHook: function () { },
    finishDynamicModuleImport: function () { },
    abortDynamicModuleImport: function () { },
    offThreadCompileToStencil: function () { },
    offThreadCompileModuleToStencil: function () { },
    offThreadDecodeStencil: function () { },
    finishOffThreadStencil: function () { },
    addPromiseReactions: function () { },
    ignoreUnhandledRejections: function () { },
    enableTrackAllocations: function () { },
    disableTrackAllocations: function () { },
    setTestFilenameValidationCallback: function () { },
  };

  for (let lfFunc in lfFuncs) {
    g[lfFunc] = lfFuncs[lfFunc];
  }

  return g;
}
