/**
 * Validates that the de-modularized server copy of assert.js
 * (src/mongo/scripting/mozjs/server/server_assert.js) is behaviorally equivalent to the shell's
 * module assert.js. Module loading is disabled in the server execution environment, so the server
 * gets a self-contained classic copy of assert.js; this guards against that copy drifting from the
 * original.
 *
 * It runs the shared assertion suite (assertions_core.js) against the server copy by installing it
 * over the shell's globals for the duration of the suite, then restoring them.
 */
import {runAssertionTests} from "jstests/libs/assertions_core.js";

// Preserve the shell's module-provided globals so they can be restored afterwards.
const saved = {};
for (const name of ["assert", "doassert", "sortDoc", "formatErrorMsg"]) {
    saved[name] = globalThis[name];
}

// Install the de-modularized server copy over the shell globals, run the shared suite against it,
// and restore the originals regardless of outcome.
// The server copy is a classic script (the server execution environment has no module
// loader), so it must be pulled in via load() rather than import.
// eslint-disable-next-line no-restricted-syntax
load("src/mongo/scripting/mozjs/server/server_assert.js");
try {
    runAssertionTests();
} finally {
    Object.assign(globalThis, saved);
}
