
import {describe, it, runTests} from "jstests/libs/mochalite.js";

describe("Date shims and polyfills", function() {
    it("should be able to create a Date", function() {
        const d = new Date(1234);
        assert.eq(d, d);
        assert.eq(d, new Date(1234));
    });

    it("timeFunc", function() {
        let hit, t;

        const func = () => {
            hit++;
        };

        hit = 0;
        t = Date.timeFunc(func);
        assert.eq(hit, 1);
        assert.gte(t, 0);  // some small number of ms

        hit = 0;
        t = Date.timeFunc(func, 4);
        assert.eq(hit, 4);
        assert.gte(t, 0);  // some small number of ms
    });

    it("tojson", function() {
        let d;

        d = new Date(0);
        assert.eq(d.tojson(), 'ISODate("1970-01-01T00:00:00Z")');

        d = new Date(1, 2, 3, 4, 5, 6, 7);
        assert.eq(d.tojson(), 'ISODate("1901-03-03T04:05:06.007Z")');
    });

    describe("ISODate", function() {
        it("should be able to create an ISO date", function() {
            let isoDate, expected;

            isoDate = ISODate("1970-01-01T00:00:00Z");
            expected = new Date(0);
            assert.eq(isoDate, expected);

            isoDate = ISODate("1970-01-01T00:00:00.000Z");
            expected = new Date(0);
            assert.eq(isoDate, expected);

            isoDate = ISODate("1901-03-03T04:05:06.007Z");
            expected = new Date(1, 2, 3, 4, 5, 6, 7);
            assert.eq(isoDate, expected);
        });

        it("should handle non-UTC timezones", function() {
            let isoDate, expected;

            isoDate = ISODate("1901-03-03T04:05:06.007+02");
            expected = new Date(1, 2, 3, 4 - 2, 5, 6, 7);
            assert.eq(isoDate, expected);

            isoDate = ISODate("1901-03-03T04:05:06.007+0200");
            expected = new Date(1, 2, 3, 4 - 2, 5, 6, 7);
            assert.eq(isoDate, expected);

            isoDate = ISODate("1901-03-03T04:05:06.007+0230");
            expected = new Date(1, 2, 3, 4 - 2, 5 - 30, 6, 7);
            assert.eq(isoDate, expected);

            isoDate = ISODate("1901-03-03T04:05:06.007+02:30");
            expected = new Date(1, 2, 3, 4 - 2, 5 - 30, 6, 7);
            assert.eq(isoDate, expected);

            isoDate = ISODate("1901-03-03T04:05:06.007-02:30");
            expected = new Date(1, 2, 3, 4 + 2, 5 + 30, 6, 7);
            assert.eq(isoDate, expected);
        });

        it("throws on bad date formats", function() {
            let e;

            e = assert.throws(() => ISODate("badpattern"));
            assert.eq(e.message, "invalid ISO date: badpattern");

            e = assert.throws(() => ISODate("0000-01-01T00:00:00.000Z-1000"));
            assert.eq(e.message, "invalid ISO date: 0000-01-01T00:00:00.000Z-1000");
        });
    });
});

runTests();
