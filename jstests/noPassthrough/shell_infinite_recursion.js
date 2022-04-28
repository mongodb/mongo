// This test checks that an infinite recursion correctly produces an 'InternalError: too much
// recursion' error and does not crash the shell.
(function() {
"use strict";

const makeBinData = () => BinData(4, "gf1UcxdHTJ2HQ/EGQrO7mQ==");
const makeUUID = () => UUID("81fd5473-1747-4c9d-8743-f10642b3bb99");
const makeHexData = () => new HexData(4, "81fd547317474c9d8743f10642b3bb99");

function infiniteRecursionGen(fn) {
    return function() {
        let testRecursiveFn = () => {
            let y = fn();
            return testRecursiveFn(y);
        };
        let x = testRecursiveFn(1);
        return x;
    };
}

function assertThrowsInfiniteRecursion(fn) {
    const err = assert.throws(fn, [], "Infinite recursion should throw an error.");
    assert(/too much recursion/.test(err.message),
           `Error wasn't caused by infinite recursion: ${err.toString()}\n${err.stack}`);

    // The choice of 20 for the number of frames is somewhat arbitrary. We check for there to be
    // some reasonable number of stack frames because most regressions would cause the stack to
    // contain a single frame or none at all.
    const kMinExpectedStack = 20;
    assert.gte(err.stack.split("\n").length,
               kMinExpectedStack,
               `Error didn't preserve the JavaScript stacktrace: ${err.toString()}\n${err.stack}`);
}

assertThrowsInfiniteRecursion(infiniteRecursionGen(makeBinData));
assertThrowsInfiniteRecursion(infiniteRecursionGen(makeUUID));
assertThrowsInfiniteRecursion(infiniteRecursionGen(makeHexData));
})();
