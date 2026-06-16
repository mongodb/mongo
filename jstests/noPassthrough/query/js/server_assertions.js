/**
 * Validates that the de-modularized server copy of assert.js
 * (src/mongo/scripting/mozjs/server/server_assert.js) is behaviorally equivalent to the shell's
 * module assert.js. Module loading is disabled in the server execution environment, so the server
 * gets a self-contained classic copy of assert.js; this guards against that copy drifting from the
 * original.
 *
 * It runs the shared assertion suite (assertions_core.js) against the server copy by installing it
 * over the shell's globals for the duration of the suite. The shared suite contains only cases
 * that are behaviorally identical between the two copies; the shell-only colorized patchDiff
 * failure-message cases live in shell_assertions.js and are not part of it. All pass/fail/throw
 * behavior is exercised identically here.
 *
 * The server copy of assert.eq omits the colorized patchDiff block that the shell appends, so its
 * failure message is just the "expected <a> to equal <b>" prefix. Those server-specific expected
 * messages are the mirror image of shell_assertions.js's colorized cases and are exercised below.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {assertThrowsError, assertThrowsErrorWithJson, kAttr, runAssertionTests} from "jstests/libs/assertions_core.js";

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
        load("src/mongo/scripting/mozjs/server/server_assert.js");
    });

    after(function () {
        Object.assign(globalThis, saved);
    });

    runAssertionTests();

    describe("server-only non-colorized assert.eq failure messages", function () {
        it("assertEqMessage", function () {
            assertThrowsError(() => {
                assert.eq(5, 2 + 2, "lorem ipsum");
            }, "expected 5 to equal 4\n : lorem ipsum");

            assertThrowsError(() => {
                assert.eq(5, 2 + 2, "lorem ipsum", kAttr);
            }, "expected 5 to equal 4\n : lorem ipsum");

            assertThrowsError(() => {
                assert.eq([["a", "c"]], [["a", "b", "c"]]);
            }, "expected [ [Array] ] to equal [ [Array] ]\n");

            assertThrowsError(() => {
                assert.eq([{a: 1, c: 3}], [{a: 1, b: 2, c: 3}]);
            }, "expected [ [Object] ] to equal [ [Object] ]\n");
        });

        it("assertEqJsonFormat", function () {
            assertThrowsErrorWithJson(
                () => {
                    assert.eq(5, 2 + 2, "lorem ipsum");
                },
                {msg: "expected 5 to equal 4\n : lorem ipsum", attr: {}},
            );
            assertThrowsErrorWithJson(
                () => {
                    assert.eq(5, 2 + 2, "lorem ipsum", kAttr);
                },
                {msg: "expected 5 to equal 4\n : lorem ipsum", attr: {...kAttr}},
            );

            assertThrowsErrorWithJson(
                () => {
                    assert.eq([["a", "c"]], [["a", "b", "c"]]);
                },
                {msg: "expected [ [Array] ] to equal [ [Array] ]\n", attr: {}},
            );

            assertThrowsErrorWithJson(
                () => {
                    assert.eq([{a: 1, c: 3}], [{a: 1, b: 2, c: 3}]);
                },
                {msg: "expected [ [Object] ] to equal [ [Object] ]\n", attr: {}},
            );
        });
    });
});
