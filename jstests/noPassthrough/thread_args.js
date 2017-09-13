/**
 * This test makes makes sure Thread and ScopedThread work with --enableJavaScriptProtection
 */
(function() {
    'use strict';
    load('jstests/libs/parallelTester.js');

    function testThread(threadType) {
        function threadFn(args) {
            // Ensure objects are passed through properly
            assert(args instanceof Object);
            // Ensure functions inside objects are still functions
            assert(args.func1 instanceof Function);
            assert(args.func1());
            // Ensure Code objects are converted to functions
            assert(args.func2 instanceof Function);
            assert(args.func2());
            // Ensure arrays are passed through properly
            assert(args.funcArray instanceof Array);
            // Ensure functions inside arrays are still functions.
            assert(args.funcArray[0] instanceof Function);
            assert(args.funcArray[0]());
            return true;
        }

        function returnTrue() {
            return true;
        }

        var args = {
            func1: returnTrue,
            // Pass some Code objects to simulate what happens with --enableJavaScriptProtection
            func2: new Code(returnTrue.toString()),
            funcArray: [new Code(returnTrue.toString())]
        };

        var thread = new threadType(threadFn, args);
        thread.start();
        thread.join();
        assert(thread.returnData());
    }

    // Test both Thread and ScopedThread
    testThread(Thread);
    testThread(ScopedThread);
}());
