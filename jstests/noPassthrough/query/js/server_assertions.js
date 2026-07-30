/**
 * Validates that the de-modularized server copy of assert.js
 * (src/mongo/scripting/mozjs/server/assert.js) is behaviorally equivalent to the shell's module
 * assert.js. Module loading is disabled in the server execution environment, so the server gets a
 * self-contained classic copy of assert.js; this guards against that copy drifting from the
 * original.
 *
 * It runs the shared assertion suite (assertions_core.js) against the server copy by installing it
 * over the shell's globals for the duration of the suite. The server copy omits the shell-only
 * colorized patchDiff failure-message formatting, so those message-format cases are skipped via
 * {colorizedDiff: false}; all pass/fail/throw behavior is exercised identically.
 */
import {after, before, describe} from "jstests/libs/mochalite.js";
import {runAssertionTests} from "jstests/libs/assertions_core.js";

describe("server-side assert.js (de-modularized classic copy)", function () {
    const saved = {};

    before(function () {
        // Preserve the shell's module-provided globals so they can be restored afterwards.
        for (const name of ["assert", "doassert", "sortDoc", "formatErrorMsg"]) {
            saved[name] = globalThis[name];
        }
        // The server copy is a classic script (the server execution environment has no module
        // loader), so it must be pulled in via load() rather than import.
        // eslint-disable-next-line no-restricted-syntax
        load("src/mongo/scripting/mozjs/server/assert.js");
    });

    after(function () {
        Object.assign(globalThis, saved);
    });

    runAssertionTests({colorizedDiff: false});
});
