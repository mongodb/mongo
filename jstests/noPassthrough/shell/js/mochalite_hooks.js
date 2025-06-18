
import {
    after,
    afterEach,
    before,
    beforeEach,
    describe,
    it,
} from "jstests/libs/mochalite.js";

// validate test execution and ordering

const log = [];

before(function() {
    log.push("before1");
    assert(typeof this, "Context");
    this.ctxBefore1 = "B1";
});
before(function() {
    log.push("before2");
    assert.eq(this.ctxBefore1, "B1");
    this.ctxBefore2 = "B2";
});

beforeEach(function() {
    log.push("--beforeEach1");
    assert(typeof this, "Context");
    assert.eq(this.ctxBefore1, "B1");
    assert.eq(this.ctxBefore2, "B2");
    this.ctxBeforeEach1 = "BE1";
});
beforeEach(function() {
    log.push("--beforeEach2");
    assert.eq(this.ctxBefore1, "B1");
    assert.eq(this.ctxBefore2, "B2");
    assert.eq(this.ctxBeforeEach1, "BE1");
    this.ctxBeforeEach2 = "BE2";
});

afterEach(function() {
    log.push("--afterEach1");
    assert(typeof this, "Context");
    assert.eq(this.ctxBefore1, "B1");
    assert.eq(this.ctxBefore2, "B2");
    assert.eq(this.ctxBeforeEach1, "BE1");
    assert.eq(this.ctxBeforeEach2, "BE2");
});
afterEach(function() {
    log.push("--afterEach2");
    assert.eq(this.ctxBefore1, "B1");
    assert.eq(this.ctxBefore2, "B2");
    assert.eq(this.ctxBeforeEach1, "BE1");
    assert.eq(this.ctxBeforeEach2, "BE2");
});

after(function() {
    log.push("after1");
    assert(typeof this, "Context");
    assert.eq(this.ctxBefore1, "B1");
    assert.eq(this.ctxBefore2, "B2");
    assert.eq(this.ctxBeforeEach1, "BE1");
    assert.eq(this.ctxBeforeEach2, "BE2");
});
after(function() {
    log.push("after2");
    assert.eq(this.ctxBefore1, "B1");
    assert.eq(this.ctxBefore2, "B2");
    assert.eq(this.ctxBeforeEach1, "BE1");
    assert.eq(this.ctxBeforeEach2, "BE2");
});

it("test1", function() {
    log.push("----test1");
    assert(typeof this, "Context");
    assert.eq(this.ctxBefore1, "B1");
    assert.eq(this.ctxBefore2, "B2");
    assert.eq(this.ctxBeforeEach1, "BE1");
    assert.eq(this.ctxBeforeEach2, "BE2");
});
it("test2", function() {
    log.push("----test2");
});

describe("describe", function() {
    before(function() {
        log.push("----describe before1");
        assert(typeof this, "Context");
        assert.eq(this.ctxBefore1, "B1");
        assert.eq(this.ctxBefore2, "B2");
        assert.eq(this.ctxBeforeEach1, "BE1");
        assert.eq(this.ctxBeforeEach2, "BE2");
        this.ctxDescribeBefore = "d>B";
    });
    before(function() {
        log.push("----describe before2");
    });

    beforeEach(function() {
        log.push("------describe beforeEach1");
        assert(typeof this, "Context");
        assert.eq(this.ctxBefore1, "B1");
        assert.eq(this.ctxBefore2, "B2");
        assert.eq(this.ctxBeforeEach1, "BE1");
        assert.eq(this.ctxBeforeEach2, "BE2");
        assert.eq(this.ctxDescribeBefore, "d>B");
        this.ctxDescribeBeforeEach = "d>BE";
    });
    beforeEach(function() {
        log.push("------describe beforeEach2");
    });

    afterEach(function() {
        log.push("------describe afterEach1");
    });
    afterEach(function() {
        log.push("------describe afterEach2");
    });

    after(function() {
        log.push("----describe after1");
    });
    after(function() {
        log.push("----describe after2");
    });

    it("test3", function() {
        log.push("--------test3");
        assert(typeof this, "Context");
        assert.eq(this.ctxBefore1, "B1");
        assert.eq(this.ctxBefore2, "B2");
        assert.eq(this.ctxBeforeEach1, "BE1");
        assert.eq(this.ctxBeforeEach2, "BE2");
        assert.eq(this.ctxDescribeBefore, "d>B");
        assert.eq(this.ctxDescribeBeforeEach, "d>BE");
    });
    it("test4", function() {
        log.push("--------test4");
    });
});

it("test5", function() {
    log.push("----test5");
});
it("test6", function() {
    log.push("----test6");

    // final check on sequencing from prior testpoints
    assert.eq(log, [
        "before1",
        "before2",
        "--beforeEach1",
        "--beforeEach2",
        "----test1",
        "--afterEach1",
        "--afterEach2",
        "--beforeEach1",
        "--beforeEach2",
        "----test2",
        "--afterEach1",
        "--afterEach2",
        "----describe before1",
        "----describe before2",
        "--beforeEach1",
        "--beforeEach2",
        "------describe beforeEach1",
        "------describe beforeEach2",
        "--------test3",
        "------describe afterEach1",
        "------describe afterEach2",
        "--afterEach1",
        "--afterEach2",
        "--beforeEach1",
        "--beforeEach2",
        "------describe beforeEach1",
        "------describe beforeEach2",
        "--------test4",
        "------describe afterEach1",
        "------describe afterEach2",
        "--afterEach1",
        "--afterEach2",
        "----describe after1",
        "----describe after2",
        "--beforeEach1",
        "--beforeEach2",
        "----test5",
        "--afterEach1",
        "--afterEach2",
        "--beforeEach1",
        "--beforeEach2",
        "----test6",
        // Outside of context - e2e resmoke tests verify these more thoroughly
        // "--afterEach1",
        // "--afterEach2",
        // "after1",
        // "after2",
    ]);
});
