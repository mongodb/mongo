
import {describe, it, runTests} from "jstests/libs/mochalite.js";

// validate test execution and ordering

const log = [];

it("test", function() {
    log.push(0);
});
it("test", function() {
    log.push(1);
});

describe("describe", function() {
    it("test", function() {
        log.push(2);
    });
    it("test", function() {
        log.push(3);
    });

    describe("describe", function() {
        it("test", function() {
            log.push(4);
        });
        it("test", function() {
            log.push(5);
        });
    });

    it("test", function() {
        log.push(6);
    });
    it("test", function() {
        log.push(7);
    });
});

it("test", function() {
    log.push(8);
});
it("test", function() {
    log.push(9);
});

assert.eq(log, []);

runTests();

assert.eq(log, [...Array(10).keys()]);
