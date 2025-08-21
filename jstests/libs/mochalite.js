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

const redText = (msg) => `\x1b[31m${msg}\x1b[0m`;
const stdout = (msg) => jsTest.log.info(msg);
const stderr = (msg) => jsTest.log.error(redText(msg));

/**
 * Reporter class for logging test results.
 * It logs passing and failing tests without throwing exceptions until the final "report" call.
 */
class Reporter {
    #passed;
    #failed;
    constructor() {
        this.reset();
    }

    /**
     * Reset the reporter state for a new test run.
     */
    reset() {
        this.#passed = [];
        this.#failed = [];
    }

    /**
     * Log a passing test/message
     * @param {string} headline
     */
    pass(headline) {
        stdout(`✔ ${headline}`);
        this.#passed.push(headline);
    }

    /**
     * Log a failing test/message
     * @param {string} headline
     * @param {Error} error
     */
    fail(headline, error) {
        stderr(`✘ ${headline}`);
        this.#failed.push({headline, error});
    }

    /**
     * Report the final test results.
     *
     * Prints a summary of passing and failing tests.
     * If there are any failures, it throws an error to signal the shell.
     * @throws {Error} if there are any failing tests
     */
    report() {
        let msg = ["Test Report Summary:", `  ${this.#passed.length} passing`];
        if (this.#failed.length > 0) {
            msg.push(redText(`  ${this.#failed.length} failing`));
            msg.push("Failures and stacks are reprinted below.");
        }
        stdout(msg.join("\n"));

        if (this.#failed.length > 0) {
            this.#failed.forEach(({headline, error}) => {
                stderr(`✘ ${headline}\n${error.message}\n${error.stack}`);
            });
            // finally throw to signal failure to the shell
            throw new Error(`${this.#failed.length} failing tests detected`);
        }
    }
}

// Context to be passed into each test and hook
class Context {}

class Scope {
    constructor(title = [], fn = null, modifiers = {only: false}) {
        this.title = title;
        this.fn = fn;
        this.only = modifiers.only;

        this.ctx = new Context();

        this.parent = null;
        this.children = [];
    }

    reset() {
        this.ctx = new Context();
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
        let children = this.children;

        // look for "only" marked scopes
        children = children.filter((child) => child.containsOnly());

        if (children.length > 0) {
            // prioritize direct it.only scopes
            let directTestOnly = children.filter((child) => child instanceof TestScope);
            if (directTestOnly.length > 0) {
                // this breaks any ties with sibling describe.only scopes
                children = directTestOnly;
            }
        } else {
            // Either no "only" was used, or came from ancestors: treat these all the same
            children = this.children;
        }

        for (const child of children) {
            let bail = await child.run();
            if (bail) {
                // If any child scope bailed out, we stop running further tests
                return;
            }
        }
    }
}

// Scope to group tests and hooks together with relevant context
// This is a Composite pattern where DescribeScope can contain other DescribeScopes or TestScopes
class DescribeScope extends Scope {
    constructor(title = [], fn = null, modifiers = {only: false}) {
        super(title, fn, modifiers);

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

        this.beforeEach = [...scope.beforeEach]; // queue

        // change shared context and invoke the content
        currScope = this;
        this.fn.call(scope.ctx);

        this.afterEach = [...this.afterEach, ...scope.afterEach]; // stack of queues
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
        return this.children.some((child) => child.containsTests());
    }

    containsOnly() {
        return this.only || this.children.some((child) => child.containsOnly());
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

        let bail = false;
        try {
            await this.runHook("before");
        } catch (error) {
            bail = true;
            reporter.fail(`"before all" hook for "${this.title}"`, error);
        }

        if (!bail) {
            await super.run();
        }

        try {
            await this.runHook("after");
        } catch (error) {
            reporter.fail(`"after all" hook for "${this.title}"`, error);
            // no explicit need to bail here, this is the end of the scope
        }

        return bail;
    }
}

// This a Leaf node in the scope tree, representing a single test's scope.
class TestScope extends Scope {
    static #titleSep = " > ";

    /**
     * Create a new TestScope for a single test.
     * @param {Test} test test element
     */
    constructor(title, fn, modifiers = {only: false}) {
        super(title, fn, modifiers);
    }

    fullTitle() {
        let titleArray = [this.title];
        let child = this;
        while (child.parent?.parent) {
            titleArray.unshift(child.parent.title);
            child = child.parent;
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

    containsOnly() {
        return this.only;
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
        const title = this.title;
        const fullTitle = this.fullTitle();

        let bail = false;
        try {
            await this.runHook("beforeEach");
        } catch (error) {
            bail = true;
            reporter.fail(`"before each" hook for "${title}"`, error);
        }

        if (!bail) {
            try {
                await this.fn.call(this.ctx);
                reporter.pass(fullTitle);
            } catch (error) {
                reporter.fail(fullTitle, error);
            }
        }

        try {
            await this.runHook("afterEach");
        } catch (error) {
            bail = true;
            reporter.fail(`"after each" hook for "${title}"`, error);
        }

        return bail;
    }
}

let currScope = new DescribeScope();
let reporter = new Reporter();

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
    addTest(title, fn, {});
}

/**
 * Variant of "it" that runs only this test case.
 * @param {string} title Test title
 * @param {function} fn Test content
 */
it.only = function (title, fn) {
    addTest(title, fn, {only: true});
};

/**
 * Variant of "it" that skips a test case.
 * @param {string} title Test title
 * @param {function} fn Test content
 */
it.skip = function (title, fn) {
    // no-op
};

function addTest(
    title,
    fn,
    options = {
        only: false,
    },
) {
    assertNoFunctionArgs(fn);
    markUsage();
    const scope = new TestScope(title, fn, options);
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
    addDescribe(title, fn, {});
}

/**
 * Variant of "describe" that runs only this test suite.
 *
 * @param {*} title
 * @param {*} fn
 */
describe.only = function (title, fn) {
    addDescribe(title, fn, {only: true});
};

/**
 * Variant of "describe" that skips a test suite.
 *
 * @param {*} title
 * @param {*} fn
 */
describe.skip = function (title, fn) {
    // no-op
};

function addDescribe(
    title,
    fn,
    options = {
        only: false,
    },
) {
    markUsage();
    const scope = new DescribeScope(title, fn, options);
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
async function runTests() {
    const rootScope = currScope;
    await currScope.run();
    try {
        reporter.report();
    } finally {
        // reset
        rootScope.reset();
        reporter.reset();
        currScope = rootScope;
    }
}

function markUsage() {
    // sentinel for shell to close
    globalThis.__mochalite_closer = runTests;
}

function assertNoFunctionArgs(fn) {
    if (fn.length > 0) {
        throw new Error(
            "Test content should not take parameters. If you intended to write callback-based content, use async functions instead.",
        );
    }
}

export {describe, it, before, beforeEach, afterEach, after};
