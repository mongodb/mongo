/**
 * Tests for the assertion functions in mongo/shell/assert.js.
 *
 * Runs the shared assertion suite (assertions_core.js) against the shell's module assert.js, then
 * adds the cases that are specific to the shell: the colorized patchDiff failure-message
 * formatting, which is absent from the de-modularized server copy and therefore not part of the
 * shared suite.
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {assertThrowsError, assertThrowsErrorWithJson, kAttr, runAssertionTests} from "jstests/libs/assertions_core.js";

runAssertionTests();

describe("shell-only colorized patchDiff failure messages", function () {
    it("assertEqMessage", function () {
        assertThrowsError(
            () => {
                assert.eq(5, 2 + 2, "lorem ipsum");
            },
            `\
expected 5 to equal 4
\u001b[32m+ expected\u001b[0m \u001b[31m- actual\u001b[0m

\u001b[31m-5\u001b[0m
\u001b[32m+4\u001b[0m
 : lorem ipsum`,
        );

        assertThrowsError(
            () => {
                assert.eq(5, 2 + 2, "lorem ipsum", kAttr);
            },
            `\
expected 5 to equal 4
\u001b[32m+ expected\u001b[0m \u001b[31m- actual\u001b[0m

\u001b[31m-5\u001b[0m
\u001b[32m+4\u001b[0m
 : lorem ipsum`,
        );

        assertThrowsError(
            () => {
                assert.eq([["a", "c"]], [["a", "b", "c"]]);
            },
            `\
expected [ [Array] ] to equal [ [Array] ]
\u001b[32m+ expected\u001b[0m \u001b[31m- actual\u001b[0m

 [
   [
     "a",
\u001b[32m+    "b",\u001b[0m
     "c"
   ]
 ]
`,
        );

        assertThrowsError(
            () => {
                assert.eq([{a: 1, c: 3}], [{a: 1, b: 2, c: 3}]);
            },
            `\
expected [ [Object] ] to equal [ [Object] ]
\u001b[32m+ expected\u001b[0m \u001b[31m- actual\u001b[0m

 [
   {
     "a" : 1,
\u001b[32m+    "b" : 2,\u001b[0m
     "c" : 3
   }
 ]
`,
        );
    });

    it("assertEqJsonFormat", function () {
        assertThrowsErrorWithJson(
            () => {
                assert.eq(5, 2 + 2, "lorem ipsum");
            },
            {
                msg: `\
expected 5 to equal 4
\u001b[32m+ expected\u001b[0m \u001b[31m- actual\u001b[0m

\u001b[31m-5\u001b[0m
\u001b[32m+4\u001b[0m
 : lorem ipsum`,
                attr: {},
            },
        );
        assertThrowsErrorWithJson(
            () => {
                assert.eq(5, 2 + 2, "lorem ipsum", kAttr);
            },
            {
                msg: `\
expected 5 to equal 4
\u001b[32m+ expected\u001b[0m \u001b[31m- actual\u001b[0m

\u001b[31m-5\u001b[0m
\u001b[32m+4\u001b[0m
 : lorem ipsum`,
                attr: {...kAttr},
            },
        );

        assertThrowsErrorWithJson(
            () => {
                assert.eq([["a", "c"]], [["a", "b", "c"]]);
            },
            {
                msg: `\
expected [ [Array] ] to equal [ [Array] ]
\u001b[32m+ expected\u001b[0m \u001b[31m- actual\u001b[0m

\u001b[31m-[ [ "a", "c" ] ]\u001b[0m
\u001b[32m+[ [ "a", "b", "c" ] ]\u001b[0m
`,
                attr: {},
            },
        );

        assertThrowsErrorWithJson(
            () => {
                assert.eq([{a: 1, c: 3}], [{a: 1, b: 2, c: 3}]);
            },
            {
                msg: `\
expected [ [Object] ] to equal [ [Object] ]
\u001b[32m+ expected\u001b[0m \u001b[31m- actual\u001b[0m

\u001b[31m-[ { "a" : 1, "c" : 3 } ]\u001b[0m
\u001b[32m+[ { "a" : 1, "b" : 2, "c" : 3 } ]\u001b[0m
`,
                attr: {},
            },
        );
    });
});
