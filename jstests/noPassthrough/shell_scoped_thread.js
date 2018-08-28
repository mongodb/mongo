/**
 * Tests ScopedThread from jstests/libs/parallelTester.js.
 */

load('jstests/libs/parallelTester.js');  // for ScopedThread

(() => {
    "use strict";

    const tests = [];

    tests.push(function checkTestData() {
        let testData = TestData;
        let worker = new ScopedThread((testData) => {
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
        let worker = new ScopedThread((testData, arg1, arg2) => {
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
            let worker = new ScopedThread((testData) => {
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
            let worker = new ScopedThread(() => {
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
            let worker = new ScopedThread(() => {
                assert.eq(TestData, undefined);
            });
            worker.start();
            worker.join();
            assert(!worker.hasFailed());
        } finally {
            TestData = oldTestData;
        }
    });

    /* main */

    tests.forEach((test) => {
        jsTest.log(`Starting tests '${test.name}'`);
        test();
    });
})();
