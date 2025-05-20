/**
 * Super simple test framework for running tests in JS files.
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
 * runTests();
 */

const nameStack = [];
const tests = [];

/**
 * Group tests together in a suite.
 * @param {string} description
 * @param {function} callback
 *
 * @example
 * describe("My Test Suite", function() {
 *     it("should do something", function() {
 *         // Test code here
 *     });
 * });
 */
export function describe(description, callback) {
    nameStack.push(description);
    callback();
    nameStack.pop();
}

/**
 *
 * @param {string} description
 * @param {function} callback
 *
 * @example
 * it("should do addition", function() {
 *     assert.eq(1 + 2, 3);
 * });
 */
export function it(description, callback) {
    let name = makeName(description, nameStack);
    tests.push({name, callback});
}

export function runTests() {
    tests.forEach(({name, callback}) => {
        try {
            callback();
            console.log(`✔ ${name}`);
        } catch (error) {
            console.error(`\x1b[31m✘ ${name}\x1b[0m\n${error}`);
            // Re-throw the error to ensure the test suite fails
            throw error;
        }
    });
}

function makeName(description, nameStack) {
    let name = nameStack.join(" > ");
    return name + (name ? " > " : "") + description;
}

// trivial polyfill
const console = {
    log: jsTest.log.info,
    error: jsTest.log.error,
};
