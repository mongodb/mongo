// This test checks that an infinite recursion correctly produces an 'InternalError: too much
// recursion' error and does not crash the server.
(function() {
"use strict";

const makeBinData = () => BinData(4, "gf1UcxdHTJ2HQ/EGQrO7mQ==");
const makeUUID = () => UUID("81fd5473-1747-4c9d-8743-f10642b3bb99");
const makeHexData = () => new HexData(4, "81fd547317474c9d8743f10642b3bb99");

function whereFnTemplate() {
    let testRecursiveFn = (i) => {
        (__fn_placeholder__)();
        testRecursiveFn(i + 1);
    };
    testRecursiveFn(0);
}

function recursiveFindWhere(db, collectionName, fn) {
    return db[collectionName].runCommand("find", {
        filter: {$where: whereFnTemplate.toString().replace("__fn_placeholder__", fn.toString())}
    });
}

function recursiveAggregateFunction(db, collectionName, fn) {
    return db.runCommand({
        "aggregate": collectionName,
        "pipeline": [{
            $addFields: {
                fld: {
                    $function: {
                        body:
                            whereFnTemplate.toString().replace("__fn_placeholder__", fn.toString()),
                        args: ["$name"],
                        lang: "js"
                    }
                }
            }
        }],
        cursor: {}
    });
}

function assertThrowsInfiniteRecursion(res) {
    assert.commandFailedWithCode(res, ErrorCodes.JSInterpreterFailure);
    assert(/too much recursion/.test(res.errmsg),
           `Error wasn't caused by infinite recursion: ${tojson(res)}`);

    // The choice of 20 for the number of frames is somewhat arbitrary. We check for there to be
    // some reasonable number of stack frames because most regressions would cause the stack to
    // contain a single frame or none at all.
    const kMinExpectedStack = 20;
    assert.gte(res.errmsg.split("\n").length,
               kMinExpectedStack,
               `Error didn't preserve the JavaScript stacktrace: ${tojson(res)}`);
}

function runTests(db, collectionName, recursiveFn) {
    assertThrowsInfiniteRecursion(recursiveFn(db, collectionName, makeBinData));
    assertThrowsInfiniteRecursion(recursiveFn(db, collectionName, makeUUID));
    assertThrowsInfiniteRecursion(recursiveFn(db, collectionName, makeHexData));
}

const collectionName = 'agg_infinite_recursion';
const collection = db[collectionName];
collection.drop();
assert.commandWorked(collection.insert({name: "Mo"}));

runTests(db, collectionName, recursiveFindWhere);
runTests(db, collectionName, recursiveAggregateFunction);
})();
