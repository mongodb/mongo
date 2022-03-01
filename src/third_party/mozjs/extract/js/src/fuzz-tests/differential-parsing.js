/* -*- Mode: javascript; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This file is used to detect and find cases where the Visage's parser is
// accepting more inputs than SpiderMonkey's parser.
//
// 1. Find new cases:
//
//   To find new cases, we have to build with libFuzzer. The JS Shell can easily
//   be built with libFuzzer by adding --enable-fuzzing to the configure script
//   command line.
//
//   Create a directory, and copy all test cases from the JS shell to this
//   directory:
//
//     $ mkdir fuzzer-input
//     $ cd fuzzer-input
//     $ find ../ -name \*.js -print0 | xargs -I '{}' -0 -n1 cp '{}' $(pwd)
//
//   Once the JS Shell is built, set the FUZZER environment variable to this
//   script location.
//
//     $ FUZZER="./fuzz-tests/differential-parsing.js" build.dir/dist/bin/js -- \
//        -use_value_profile=1 -print_pcs=1 -timeout=5 -max_len=32 -only_ascii=1 \
//        ./fuzzer-input
//
// 2. Test a crashing test case:
//
//   Once a new crashing test case is found, this script can be used to
//   reproduce the crashing conditions.
//
//   To do so, you need a JS Shell and to load this script and use the testFile
//   function with the location of the crashing file.
//
//     $ build.dir/dist/bin/js
//     js> load("./fuzz-tests/differential-parsing.js");
//     js> testFile("./crash-42");
//     Parse Script C++: fail
//     Parse Module C++: fail
//     Parse Script Rust: succeed
//     Parse Module Rust: fail
//     Hit MOZ_CRASH(Rust accept more than C++)
//

/* global crash, os, parse, timeout */

// This global will hold the current fuzzing buffer for each iteration.
var fuzzBuf;

function timed(sec, f) {
  // If the function `f` takes more than 3 seconds, then the evaluation ends
  // prematurely and returns in libFuzzer handler without considering this
  // test case as interesting.
  timeout(sec, function() {
    return false;
  });
  f();

  // Remove the timeout handler, to not kill future executions.
  timeout(-1);
}

var parseScriptCpp = { module: false, smoosh: false };
var parseScriptRust = { module: false, smoosh: true };
var parseModuleRust = { module: true, smoosh: true };
var parseModuleCpp = { module: true, smoosh: false };
function test(code, verbose = false) {
  var isScriptCpp = false,
    isModuleCpp = false,
    isScriptRust = false,
    isModuleRust = false;
  try {
    parse(code, parseScriptCpp);
    isScriptCpp = true;
    if (verbose) {
      console.log("Parse Script C++: succeed");
    }
  } catch (exc) {
    if (verbose) {
      console.log("Parse Script C++: fail");
    }
  }
  try {
    parse(code, parseModuleCpp);
    isModuleCpp = true;
    if (verbose) {
      console.log("Parse Module C++: succeed");
    }
  } catch (exc) {
    if (verbose) {
      console.log("Parse Module C++: fail");
    }
  }
  try {
    parse(code, parseScriptRust);
    isScriptRust = true;
    if (verbose) {
      console.log("Parse Script Rust: succeed");
    }
  } catch (exc) {
    if (verbose) {
      console.log("Parse Script Rust: fail");
    }
  }
  try {
    parse(code, parseModuleRust);
    isModuleRust = true;
    if (verbose) {
      console.log("Parse Module Rust: succeed");
    }
  } catch (exc) {
    if (verbose) {
      console.log("Parse Module Rust: fail");
    }
  }
  if ((isScriptRust && !isScriptCpp) || (isModuleRust && !isModuleCpp)) {
    crash("Rust accept more than C++");
  }
}

function JSFuzzIterate() {
  // This function is called per iteration. You must ensure that:
  //
  //   1) Each of your actions/decisions is only based on fuzzBuf,
  //      in particular not on Math.random(), Date/Time or other
  //      external inputs.
  //
  //   2) Your actions should be deterministic. The same fuzzBuf
  //      should always lead to the same set of actions/decisions.
  //
  //   3) You can modify the global where needed, but ensure that
  //      each iteration is isolated from one another by cleaning
  //      any modifications to the global after each iteration.
  //      In particular, iterations must not depend on or influence
  //      each other in any way (see also 1)).
  //
  //   4) You must catch all exceptions.
  let code = String.fromCharCode(...fuzzBuf);
  timed(3, _ => test(code));
  return 0;
}

function testFile(file) {
  let content = os.file.readFile(file);
  test(content, true);
}
