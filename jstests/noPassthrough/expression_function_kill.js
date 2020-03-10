/**
 * Tests where/function can be interrupted through maxTimeMS and query knob.
 */
(function() {
"use strict";

const mongodOptions = {};
const conn = MongoRunner.runMongod(mongodOptions);

let db = conn.getDB("test_where_function_interrupt");
let coll = db.getCollection("foo");

let expensiveFunction = function() {
    sleep(1000);
    return true;
};
assert.commandWorked(coll.insert(Array.from({length: 1000}, _ => ({}))));

let checkInterrupt = function(cursor) {
    let err = assert.throws(function() {
        cursor.itcount();
    }, [], "expected interrupt error due to maxTimeMS being exceeded");
    assert.commandFailedWithCode(
        err, [ErrorCodes.MaxTimeMSExpired, ErrorCodes.Interrupted, ErrorCodes.InternalError]);
};

let tests = [
    {
        // Test that $where can be interrupted with a maxTimeMS of 100 ms.
        timeout: 100,
        query: {$where: expensiveFunction},
        err: checkInterrupt,
    },
    {
        // Test that $function can be interrupted with a maxTimeMS of 100 ms.
        timeout: 100,
        query: {
            $expr: {
                $function: {
                    body: expensiveFunction,
                    args: [],
                    lang: 'js',
                }
            }
        },
        err: checkInterrupt
    },
    {

        // Test that $function can be interrupted by a query knob of 100 ms.
        pre: function() {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryJavaScriptFnTimeoutMillis: 100}));
        },
        query: {
            $expr: {
                $function: {
                    body: expensiveFunction,
                    args: [],
                    lang: 'js',
                }
            }
        },
        err: checkInterrupt
    },
];

tests.forEach(function(testCase) {
    if (testCase.pre) {
        testCase.pre();
    }

    let cursor = coll.find(testCase.query);

    if (testCase.timeout) {
        cursor.maxTimeMS(testCase.timeout);
    }
    testCase.err(cursor);
});

MongoRunner.stopMongod(conn);
})();
