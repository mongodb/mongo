/* -*- Mode: javascript; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This fuzzing target aims to stress the SpiderMonkey parser. However, for
// this purpose, it does *not* use `parse()` because some past bugs in the
// parser could only be triggered in the runtime later. Instead, we use
// the `evaluate` function which parses and runs the code. This brings in
// other problems like timeouts and permanent side-effects. We try to minimize
// the amount of permanent side-effects from running the code by running it
// in a fresh global for each iteration. We also use a special function
// called `sanitizeGlobal` to remove any harmful shell functions from the
// global prior to running. Many of these shell functions would otherwise
// have permanent side-effects of some sort or be disruptive to testing like
// increasing the amount of timeouts or leak memory. Finally, the target also
// tries to catch timeouts locally and signal back any timeouts by returning 1
// from the iteration function.

// This global will hold the current fuzzing buffer for each iteration.
var fuzzBuf;

loadRelativeToScript("util/sanitize.js");

deterministicgc(true);

// Set a default value for timeouts to 1 second, but allow this to
// be set on the command line as well using -e fuzzTimeout=VAL.
if (typeof fuzzTimeout === "undefined") {
  fuzzTimeout = 1;
}

function JSFuzzIterate() {
  try {
    let code = String.fromCharCode(...fuzzBuf);
    let result = null;

    // Create a new global and sanitize it such that its potentially permanent
    // side-effects are reduced to a minimum.
    let global = newGlobal();
    sanitizeGlobal(global);

    // Work around memory leaks when the hook is not set
    evaluate(`
      setModuleResolveHook(function(module, specifier) {
        throw "Module '" + specifier + "' not found";
      });
      setModuleResolveHook = function() {};
    `, { global: global, catchTermination: true });

    // Start a timer and set a timeout in addition
    let lfStart = monotonicNow();
    timeout(fuzzTimeout, function() { return false; });

    try {
      result = evaluate(code, { global: global, catchTermination: true });
    } catch(exc) {
      print(exc);
    }

    timeout(-1);
    let lfStop = monotonicNow();

    // Reset some things that could have been altered by the code we ran
    gczeal(0);
    schedulegc(0);
    setGCCallback({ action: "majorGC" });
    clearSavedFrames();

    // If we either ended terminating the script, or we took longer than
    // the timeout set (but timeout didn't kick in), then we return 1 to
    // signal libFuzzer that the sample just be abandoned.
    if (result === "terminated" || (lfStop - lfStart > (fuzzTimeout * 1000 + 200))) {
      return 1;
    }

    return 0;
  } catch(exc) {
    print("Caught toplevel exception: " + exc);
  }

  return 1;
}
