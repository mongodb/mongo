
import {describe, it, runTests} from "jstests/libs/mochalite.js";

describe("Timestamp shims and polyfills", function() {
    it("should be able to create a Timestamp", function() {
        const ts = new Timestamp(1, 2);
        assert.eq(ts, ts);
        assert.eq(ts, new Timestamp(1, 2));
    });

    it("getTime", function() {
        const ts = new Timestamp(1, 2);
        assert.eq(ts.getTime(), 1);
    });

    it("getInc", function() {
        const ts = new Timestamp(1, 2);
        assert.eq(ts.getInc(), 2);
    });

    it("toString", function() {
        const ts = new Timestamp(1, 2);
        // Resmoke overrides `toString` to throw an error to prevent accidental operator
        // comparisons, e.g: >, -, etc...
        const e = assert.throws(ts.toString);
        assert.eq(
            e.message,
            "Cannot toString timestamps. Consider using timestampCmp() for comparison or tojson(<variable>) for output.");
    });

    it("tojson", function() {
        const ts = new Timestamp(1, 2);
        assert.eq(ts.tojson(), "Timestamp(1, 2)");
    });

    it("toStringIncomparable", function() {
        const ts = new Timestamp(1, 2);
        assert.eq(ts.toStringIncomparable(), "Timestamp(1, 2)");
    });
});

runTests();
