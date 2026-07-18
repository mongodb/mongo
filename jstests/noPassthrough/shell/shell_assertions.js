/**
 * Tests for the assertion functions in mongo/shell/assert.js.
 *
 * Runs the shared assertion suite (assertions_core.js) against the shell's module assert.js, which
 * includes the colorized patchDiff failure-message formatting.
 */
import {runAssertionTests} from "jstests/libs/assertions_core.js";

runAssertionTests({colorizedDiff: true});
