import {describe, it} from "jstests/libs/mochalite.js";

describe("Timestamp shims and polyfills", function () {
    it("should be able to create a Timestamp", function () {
        const ts = new Timestamp(1, 2);
        assert.eq(ts, ts);
        assert.eq(ts, new Timestamp(1, 2));
    });

    it("Constructs without 'new'", () => {
        let a = new Timestamp(10, 20);
        let b = Timestamp(a.t, a.i);
        printjson(a);
        assert.eq(tojson(a), tojson(b), "timestamp");
    });

    it("edge-case inputs", () => {
        assert.throws(() => Timestamp(-2, 3), [], "Timestamp time must not accept negative time");
        assert.throws(() => Timestamp(0, -1), [], "Timestamp increment must not accept negative time");
        assert.throws(
            () => Timestamp(0x10000 * 0x10000, 0),
            [],
            "Timestamp time must not accept values larger than 2**32 - 1",
        );
        assert.throws(
            () => Timestamp(0, 0x10000 * 0x10000),
            [],
            "Timestamp increment must not accept values larger than 2**32 - 1",
        );

        let a = new Timestamp(0x80008000, 0x80008000 + 0.5);
        let b = Timestamp(a.t, Math.round(a.i));
        printjson(a);
        assert.eq(tojson(a), tojson(b), "timestamp");
    });

    it("getTime", function () {
        const ts = new Timestamp(1, 2);
        assert.eq(ts.getTime(), 1);
    });

    it("getInc", function () {
        const ts = new Timestamp(1, 2);
        assert.eq(ts.getInc(), 2);
    });

    it("toString", function () {
        const ts = new Timestamp(1, 2);
        // Resmoke overrides `toString` to throw an error to prevent accidental operator
        // comparisons, e.g: >, -, etc...
        const e = assert.throws(ts.toString);
        assert.eq(
            e.message,
            "Cannot toString timestamps. Consider using timestampCmp() for comparison or tojson(<variable>) for output.",
        );
    });

    it("tojson", function () {
        const ts0 = new Timestamp();
        assert.eq(ts0.tojson(), "Timestamp(0, 0)");

        const ts = new Timestamp(1, 2);
        assert.eq(ts.tojson(), "Timestamp(1, 2)");
        assert.eq(toJsonForLog(ts, "", true), '{"$timestamp":{"t":1,"i":2}}');
    });

    it("toStringIncomparable", function () {
        const ts = new Timestamp(1, 2);
        assert.eq(ts.toStringIncomparable(), "Timestamp(1, 2)");
    });
});
