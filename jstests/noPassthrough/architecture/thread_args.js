/**
 * This test makes makes sure Thread works with --enableJavaScriptProtection
 */
import {Thread} from "jstests/libs/parallelTester.js";

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

    let args = {
        func1: returnTrue,
        // Pass some Code objects to simulate what happens with --enableJavaScriptProtection
        func2: new Code(returnTrue.toString()),
        funcArray: [new Code(returnTrue.toString())],
    };

    let thread = new threadType(threadFn, args);
    thread.start();
    thread.join();
    assert(thread.returnData());
}

// Test the Thread class
testThread(Thread);
