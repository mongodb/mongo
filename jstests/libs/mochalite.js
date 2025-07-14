/**
 * Simple test framework for running tests in JS files.
 *
 * This is a simplified version of Mocha, designed to work with the Mongo shell.
 * It provides a way to define test suites and individual tests, and to run them with
 * a simple reporting format.
 *
 * Example usage:
 *
 *    describe("My Test Suite", function() {
 *        it("should do something", function() {
 *            // Test code here
 *        });
 *    });
 *
 *
 * Leverage the before/beforeEach/afterEach/after hooks to set up and tear down test environments.
 *
 * Example:
 *
 *    before(function() {
 *        this.fixtureDB = startupNewDB();
 *    });
 *    beforeEach(function() {
 *        this.fixtureDB.seed();
 *    });
 *    afterEach(function() {
 *        this.fixtureDB.clear();
 *    });
 *    after(function() {
 *        this.fixtureDB.shutdown();
 *    });
 *    it("should do something", function() {
 *        this.fixtureDB.insert({ name: "test" });
 *        assert.eq(this.fixtureDB.find({ name: "test" }).count(), 1);
 *    });
 *
 * Content in any of the above (excluding `describe`) can be an async function (or otherwise return
 * a Promise) and the framework will await its resolution.
 *
 * Example:
 *
 *    before(async function() {
 *        this.fixtureDB = await startupNewDBAsync();
 *    });
 *    it("should do something", async function() {
 *        await this.fixtureDB.insertAsync({ name: "test" });
 *        await res = this.fixtureDB.find({ name: "test" }).count();
 *        assert.eq(res, 1);
 *    });
 *    after(function() {
 *        return this.fixtureDB.shutdownAsync(); // returns a promise
 *    });
 */

// use grep from global context if available, else match everything
let GREP = globalThis._mocha_grep ?? ".*";
try {
    GREP = new RegExp(GREP);
} catch (e) {
    throw new Error(`Failed to create regex from '${GREP}': ${e.message}`);
}

class Printer {
    static printPass(title) {
        jsTest.log.info(`✔ ${title}`);
    }

    static printFailure(title, error) {
        const red = msg => `\x1b[31m${msg}\x1b[0m`;
        jsTest.log.error(red(`✘ ${title}`), {error});
    }
}

// Context to be passed into each test and hook
class Context {}

class Scope {
    constructor(title = [], fn = null) {
        this.title = title;
        this.fn = fn;

        this.ctx = new Context();

        this.parent = null;
        this.children = [];
    }

    /**
     * Add a child scope to this scope.
     * @param {Scope} scope
     */
    addChild(scope) {
        scope.ctx = this.ctx;
        scope.parent = this;
        this.children.push(scope);
    }

    /**
     * Run all child scopes, async but serially.
     * @async
     */
    async run() {
        for (const child of this.children) {
            await child.run();
        }
    }
}

// Scope to group tests and hooks together with relevant context
// This is a Composite pattern where DescribeScope can contain other DescribeScopes or TestScopes
class DescribeScope extends Scope {
    constructor(title = [], fn = null) {
        super(title, fn);

        // hooks
        this.before = [];
        this.beforeEach = [];
        this.afterEach = [];
        this.after = [];
    }

    /**
     * Discover and gather nested content (nested hooks, describe, and it calls)
     * @param {*} scope Current scope to discover from
     * @param {string} title Title of this scope
     * @param {function} fn Function to execute in this scope
     */
    discover(scope) {
        this.ctx = scope.ctx;

        this.beforeEach = [...scope.beforeEach];  // queue

        // change shared context and invoke the content
        currScope = this;
        this.fn.call(scope.ctx);

        this.afterEach = [...this.afterEach, ...scope.afterEach];  // stack of queues
    }

    /**
     * Add a hook to this scope.
     * @param {string} hookname
     * @param {Function} fn
     */
    addHook(hookname, fn) {
        assertNoFunctionArgs(fn);
        this[hookname].push(fn);
    }

    /**
     * Check if this scope or any nested scopes contain tests.
     * @returns {boolean}
     */
    containsTests() {
        return this.children.some(node => node.containsTests());
    }

    /**
     * Run all hooks of the given type for this scope.
     * @param {string} hookname
     * @async
     */
    async runHook(hookname) {
        const ctx = this.ctx;
        for (const fn of this[hookname]) {
            await fn.call(ctx);
        }
    }

    /**
     * Run the before-content-after workflow
     * @async
     */
    async run() {
        if (!this.containsTests()) {
            // no tests in this scope or nested scopes, skip running any hooks
            return;
        }
        await this.runHook("before");
        await super.run();
        await this.runHook("after");
    }
}

// This a Leaf node in the scope tree, representing a single test's scope.
class TestScope extends Scope {
    static #titleSep = " > ";

    /**
     * Create a new TestScope for a single test.
     * @param {Test} test test element
     */
    constructor(title, fn) {
        super(title, fn);
    }

    fullTitle() {
        let titleArray = [this.title];
        let node = this;
        while (node.parent?.parent) {
            titleArray.unshift(node.parent.title);
            node = node.parent;
        }
        return titleArray.join(TestScope.#titleSep);
    }

    /**
     * Check if this scope contains tests to run.
     * @returns {boolean}
     */
    containsTests() {
        return GREP.test(this.fullTitle());
    }

    /**
     * Run a specific hook for this scope.
     * @param {string} hookname
     * @async
     */
    async runHook(hookname) {
        // defer to the parent scope
        await this.parent.runHook(hookname);
    }

    /**
     * Run the beforeEach-test-afterEach workflow
     * @async
     */
    async run() {
        const title = this.fullTitle();
        try {
            await this.runHook("beforeEach");
            await this.fn.call(this.ctx);
            Printer.printPass(title);
        } catch (error) {
            Printer.printFailure(title, error);
            // Re-throw the error to ensure the test suite fails
            throw error;
        } finally {
            await this.runHook("afterEach");
        }
    }
}

let currScope = new DescribeScope();

/**
 * Define a test case.
 * @param {string} title Test title
 * @param {function} fn Test content
 *
 * @example
 * it("should do addition", function() {
 *     assert.eq(1 + 2, 3);
 * });
 *
 * @example
 * it("should do async addition", async function() {
 *     const result = await asyncAdd(1, 2);
 *     assert.eq(result, 3);
 * });
 */
function it(title, fn) {
    assertNoFunctionArgs(fn);
    markUsage();
    const scope = new TestScope(title, fn);
    currScope.addChild(scope);
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
    const scope = new DescribeScope(title, fn);
    const oldScope = currScope;
    scope.discover(currScope);
    oldScope.addChild(scope);
    currScope = oldScope;
}

/**
 * Run a function before all tests in the current scope.
 * @param {Function} fn Function to invoke
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
 * @param {Function} fn Function to invoke
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
 * @param {Function} fn Function to invoke
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
 * @param {Function} fn Function to invoke
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
 * Returns a Promise.
 */
function runTests() {
    return currScope.run();
}

function markUsage() {
    // sentinel for shell to close
    globalThis.__mochalite_closer = runTests;
}

function assertNoFunctionArgs(fn) {
    if (fn.length > 0) {
        throw new Error(
            "Test content should not take parameters. If you intended to write callback-based content, use async functions instead.");
    }
}

export {describe, it, before, beforeEach, afterEach, after};
