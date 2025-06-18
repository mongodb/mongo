import {describe, it} from "jstests/libs/mochalite.js";

describe("String shims and polyfills", function() {
    it("should implement trim", function() {
        const str = "  hello  ";
        assert.eq(str.trim(), "hello");
    });

    it("should implement trimLeft", function() {
        const str = "  hello  ";
        assert.eq(str.trimLeft(), "hello  ");
    });

    it("should implement trimRight", function() {
        const str = "  hello  ";
        assert.eq(str.trimRight(), "  hello");
    });

    it("should implement ltrim", function() {
        const str = "  hello  ";
        assert.eq(str.ltrim(), "hello  ");
    });

    it("should implement rtrim", function() {
        const str = "  hello  ";
        assert.eq(str.rtrim(), "  hello");
    });

    it("should implement startsWith", function() {
        const str = "hello world";
        assert.eq(str.startsWith("hello"), true);
        assert.eq(str.startsWith("world"), false);
    });

    it("should implement endsWith", function() {
        const str = "hello world";
        assert.eq(str.endsWith("hello"), false);
        assert.eq(str.endsWith("world"), true);
    });

    it("should implement includes", function() {
        const str = "applebananacherry  ";
        assert.eq(str.includes("banana"), true);
        assert.eq(str.includes("date"), false);
    });

    it("should implement pad", function() {
        const str = "hello";
        assert.eq(str.pad(10), "     hello");
        assert.eq(str.pad(10, true), "hello     ");
        assert.eq(str.pad(10, false, "X"), "XXXXXhello");
        assert.eq(str.pad(10, true, "X"), "helloXXXXX");
    });
});
