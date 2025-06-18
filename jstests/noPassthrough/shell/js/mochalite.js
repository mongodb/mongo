
import {describe, it} from "jstests/libs/mochalite.js";

// validate test execution and ordering

const log = [];

it("test0", function() {
    log.push(0);
});
it("test1", function() {
    log.push(1);
});

describe("describeA", function() {
    it("test2", function() {
        log.push(2);
    });
    it("test3", function() {
        log.push(3);
    });

    describe("describeB", function() {
        it("test4", function() {
            log.push(4);
        });
        it("test5", function() {
            log.push(5);
        });
    });

    it("test6", function() {
        log.push(6);
    });
    it("test7", function() {
        log.push(7);
    });
});

it("test8", function() {
    log.push(8);
});
it("test9", function() {
    log.push(9);

    // final check on sequencing from prior testpoints
    assert.eq(log, [...Array(10).keys()]);
});
