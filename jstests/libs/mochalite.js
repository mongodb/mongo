/**
 * Simple test framework for running tests in JS files.
 *
 * This is a simplified version of Mocha, designed to work with the Mongo shell.
 * It provides a way to define test suites and individual tests, and to run them with
 * a simple reporting format.
 *
 * Example usage:
 *
 * describe("My Test Suite", function() {
 *     it("should do something", function() {
 *         // Test code here
 *     }
 * });
 *
 *
 * Leverage the before/beforeEach/afterEach/after hooks to set up and tear down test environments.
 *
 * Example:
 * before(function() {
 *     this.fixtureDB = startupNewDB();
 * });
 * beforeEach(function() {
 *     this.fixtureDB.seed();
 * });
 * afterEach(function() {
 *     this.fixtureDB.clear();
 * });
 * after(function() {
 *     this.fixtureDB.shutdown();
 * });
 * it("should do something", function() {
 *     this.fixtureDB.insert({ name: "test" });
 *     assert.eq(this.fixtureDB.find({ name: "test" }).count(), 1);
 * });
 */

// Context to be passed into each test and hook
class Context {}

// Test to be run with a given Context
class Test {
    _titleArray;
    _fn;
    static _titleSep = " > ";

    constructor(title, fn) {
        this._titleArray = title;
        this._fn = fn;
    }

    fullTitle() {
        return this._titleArray.join(Test._titleSep);
    }

    /**
     * Invoke the test function with the given context.
     * @param {Context} ctx
     */
    run(ctx) {
        this._fn.call(ctx);
    }

    printPass() {
        jsTest.log.info(`✔ ${this.fullTitle()}`);
    }

    printFailure(error) {
        jsTest.log.error(`\x1b[31m✘ ${this.fullTitle()}\x1b[0m`, {error});
    }
}

// Scope to group tests and hooks together with relevant context
class Scope {
    constructor() {
        this.title = [];
        this.content = [];
        this.ctx = new Context();

        // hooks
        this.before = [];
        this.beforeEach = [];
        this.afterEach = [];
        this.after = [];
    }

    static inherit(oldScope) {
        const newScope = new Scope();
        newScope.title = [...oldScope.title];
        newScope.beforeEach = [...oldScope.beforeEach];  // queue
        newScope.ctx = oldScope.ctx;                     // inherit context
        return newScope;
    }

    addContent(block) {
        this.content.push(block);
    }

    addHook(hookname, fn) {
        this[hookname].push(fn);
    }

    runHook(hookname) {
        const ctx = this.ctx;
        this[hookname].forEach(function(fn) {
            fn.call(ctx);
        });
    }

    /**
     * Run the before-content-after workflow
     */
    run() {
        this.runHook("before");
        for (const node of this.content) {
            if (node instanceof Test) {
                this.runTest(node);
            } else {
                // a nested scope (describe block)
                node.run();
            }
        }
        this.runHook("after");
    }

    /**
     * Run the beforeEach-test-afterEach workflow for a given test.
     * @param {Test} test
     */
    runTest(test) {
        try {
            this.runHook("beforeEach");
            test.run(this.ctx);
            test.printPass();
        } catch (error) {
            test.printFailure(error);
            // Re-throw the error to ensure the test suite fails
            throw error;
        } finally {
            this.runHook("afterEach");
        }
    }
}

let currScope = new Scope();

function addTest(title, fn) {
    const test = new Test(
        [...currScope.title, title],
        fn,
    );
    currScope.addContent(test);
}

function addScope(title, fn) {
    const oldScope = currScope;
    currScope = Scope.inherit(oldScope);
    currScope.title.push(title);

    fn.call(oldScope.ctx);

    currScope.afterEach = [...currScope.afterEach, ...oldScope.afterEach];  // stack of queues
    oldScope.addContent(currScope);
    currScope = oldScope;
}

/**
 * Define a test case.
 * @param {string} title
 * @param {function} fn
 *
 * @example
 * it("should do addition", function() {
 *     assert.eq(1 + 2, 3);
 * });
 */
function it(title, fn) {
    markUsage();
    addTest(title, fn);
}

/**
 * Group tests together in a suite.
 * @param {string} title
 * @param {function} fn
 *
 * @example
 * describe("My Test Suite", function() {
 *     it("should do something", function() {
 *         // Test code here
 *     });
 * });
 */
function describe(title, fn) {
    markUsage();
    addScope(title, fn);
}

/**
 * Run a function before all tests in the current scope.
 * @param {Function} fn
 * @example
 * before(function() {
 *     this.fixtureDB = startupNewDB();
 * });
 * it("should do something", function() {
 *    this.fixtureDB.insert({ name: "test" });
 *    // ...
 * });
 */
function before(fn) {
    currScope.addHook("before", fn);
}

/**
 * Run a function before each test in the current scope.
 * @param {Function} fn
 * @example
 * beforeEach(function() {
 *     this.fixtureDB = startupNewDB();
 * });
 * it("should do something", function() {
 *    this.fixtureDB.insert({ name: "test" });
 *    // ...
 * });
 */
function beforeEach(fn) {
    currScope.addHook("beforeEach", fn);
}

/**
 * Run a function after each test in the current scope.
 * @param {Function} fn
 * @example
 * beforeEach(function() {
 *     this.fixtureDB = startupNewDB();
 * });
 * afterEach(function() {
 *     this.fixtureDB.shutdown();
 * });
 * it("should do something", function() {
 *    this.fixtureDB.insert({ name: "test" });
 *    // ...
 * });
 */
function afterEach(fn) {
    currScope.addHook("afterEach", fn);
}

/**
 * Run a function after all tests in the current scope.
 * @param {Function} fn
 * @example
 * before(function() {
 *     this.fixtureDB = startupNewDB();
 * });
 * after(function() {
 *     this.fixtureDB.shutdown();
 * });
 * it("should do something", function() {
 *    this.fixtureDB.insert({ name: "test" });
 *    // ...
 * });
 */
function after(fn) {
    currScope.addHook("after", fn);
}

/**
 * Run all defined tests.
 */
function runTests() {
    currScope.run();
}

function markUsage() {
    // sentinel for shell to close
    globalThis.__mochalite_closer = runTests;
}

export {describe, it, before, beforeEach, afterEach, after};
