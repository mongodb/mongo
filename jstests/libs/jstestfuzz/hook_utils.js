// Utility for defining hooks that get run at various points the fuzzer generated files.
//
// before/afterServerInfo hooks run before and after the fuzzer preamble reaches out to the server
// for information. Use these hooks to prevent server commands in preamble.js from failing.

function defineFuzzerHooks({
    beforeServerInfo: beforeServerInfo = Function.prototype,
    afterServerInfo: afterServerInfo = Function.prototype,
} = {}) {
    if (typeof TestData === 'undefined') {
        throw new Error('jstestfuzz tests must be run through resmoke.py');
    }

    TestData.beforeFuzzerServerInfoHooks = TestData.beforeFuzzerServerInfoHooks || [];
    TestData.afterFuzzerServerInfoHooks = TestData.afterFuzzerServerInfoHooks || [];

    TestData.beforeFuzzerServerInfoHooks.push(beforeServerInfo);
    TestData.afterFuzzerServerInfoHooks.push(afterServerInfo);
}
