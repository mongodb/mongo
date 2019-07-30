/**
 * Tests Thread from jstests/libs/parallelTester.js.
 */

load('jstests/libs/parallelTester.js');  // for Thread

(() => {
    "use strict";

    const tests = [];

    tests.push(function checkTestData() {
        let testData = TestData;
        let worker = new Thread((testData) => {
            assert.eq(TestData, testData);
        }, testData);
        worker.start();
        worker.join();
        assert(!worker.hasFailed());
    });

    tests.push(function checkTestDataWithOtherArgs() {
        let testData = TestData;
        let arg1 = 1;
        let arg2 = {a: 1};
        let worker = new Thread((testData, arg1, arg2) => {
            assert.eq(TestData, testData);
            assert.eq(arg1, 1);
            assert.eq(arg2, {a: 1});
        }, testData, arg1, arg2);
        worker.start();
        worker.join();
        assert(!worker.hasFailed());
    });

    tests.push(function checkTestDataWithFunc() {
        let oldTestData = TestData;
        if (!TestData) {
            TestData = {};
        }
        TestData.func = function myfunc(x) {
            return x;
        };
        let testData = TestData;
        try {
            let worker = new Thread((testData) => {
                // We cannot directly compare testData & TestData because the func object
                // has extra whitespace and line control.
                assert.eq(Object.keys(TestData), Object.keys(testData));
                for (var property in TestData) {
                    if (TestData.hasOwnProperty(property) && !TestData.property instanceof Code) {
                        assert.eq(TestData.property, testData.property);
                    }
                }
                assert.eq(testData.func(7), 7);
                assert.eq(TestData.func(7), 7);
            }, testData);
            worker.start();
            worker.join();
            assert(!worker.hasFailed());
        } finally {
            TestData = oldTestData;
        }
    });

    tests.push(function nullTestData() {
        let oldTestData = TestData;
        TestData = null;
        try {
            let worker = new Thread(() => {
                assert.eq(TestData, null);
            });
            worker.start();
            worker.join();
            assert(!worker.hasFailed());
        } finally {
            TestData = oldTestData;
        }
    });

    tests.push(function undefinedTestData() {
        let oldTestData = TestData;
        TestData = undefined;
        try {
            let worker = new Thread(() => {
                assert.eq(TestData, undefined);
            });
            worker.start();
            worker.join();
            assert(!worker.hasFailed());
        } finally {
            TestData = oldTestData;
        }
    });

    function testUncaughtException(joinFn) {
        const thread = new Thread(function myFunction() {
            throw new Error("Intentionally thrown inside Thread");
        });
        thread.start();

        let error = assert.throws(joinFn, [thread]);
        assert(/Intentionally thrown inside Thread/.test(error.message),
               () => "Exception didn't include the message from the exception thrown in Thread: " +
                   tojson(error.message));
        assert(/myFunction@/.test(error.stack),
               () => "Exception doesn't contain stack frames from within the Thread: " +
                   tojson(error.stack));
        assert(/testUncaughtException@/.test(error.stack),
               () => "Exception doesn't contain stack frames from caller of the Thread: " +
                   tojson(error.stack));

        error = assert.throws(() => thread.join());
        assert.eq("Thread not running",
                  error.message,
                  "join() is expected to be called only once for the thread");

        assert.eq(true,
                  thread.hasFailed(),
                  "Uncaught exception didn't cause thread to be marked as having failed");
        assert.doesNotThrow(() => thread.returnData(),
                            [],
                            "returnData() threw an exception after join() had been called");
        assert.eq(undefined,
                  thread.returnData(),
                  "returnData() shouldn't have anything to return if the thread failed");
    }

    tests.push(function testUncaughtExceptionAndWaitUsingJoin() {
        testUncaughtException(thread => thread.join());
    });

    // The returnData() method internally calls the join() method and should also throw an exception
    // if the Thread had an uncaught exception.
    tests.push(function testUncaughtExceptionAndWaitUsingReturnData() {
        testUncaughtException(thread => thread.returnData());
    });

    tests.push(function testUncaughtExceptionInNativeCode() {
        const thread = new Thread(function myFunction() {
            new Timestamp(-1);
        });
        thread.start();

        const error = assert.throws(() => thread.join());
        assert(/Timestamp/.test(error.message),
               () => "Exception didn't include the message from the exception thrown in Thread: " +
                   tojson(error.message));
        assert(/myFunction@/.test(error.stack),
               () => "Exception doesn't contain stack frames from within the Thread: " +
                   tojson(error.stack));
    });

    tests.push(function testUncaughtExceptionFromNestedThreads() {
        const thread = new Thread(function myFunction1() {
            load("jstests/libs/parallelTester.js");

            const thread = new Thread(function myFunction2() {
                load("jstests/libs/parallelTester.js");

                const thread = new Thread(function myFunction3() {
                    throw new Error("Intentionally thrown inside Thread");
                });

                thread.start();
                thread.join();
            });

            thread.start();
            thread.join();
        });
        thread.start();

        const error = assert.throws(() => thread.join());
        assert(/Intentionally thrown inside Thread/.test(error.message),
               () => "Exception didn't include the message from the exception thrown in Thread: " +
                   tojson(error.message));
        assert(/myFunction3@/.test(error.stack),
               () => "Exception doesn't contain stack frames from within the innermost Thread: " +
                   tojson(error.stack));
        assert(/myFunction2@/.test(error.stack),
               () => "Exception doesn't contain stack frames from within an inner Thread: " +
                   tojson(error.stack));
        assert(/myFunction1@/.test(error.stack),
               () => "Exception doesn't contain stack frames from within the outermost Thread: " +
                   tojson(error.stack));
    });

    /* main */

    tests.forEach((test) => {
        jsTest.log(`Starting tests '${test.name}'`);
        test();
    });
})();
