
import {describe, it} from "jstests/libs/mochalite.js";

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

    it("tojson on incomplete dates", function() {
        // these are maybe unexpected, but specify older behavior to retain compatibility
        let d;

        d = new Date(NaN);
        assert.eq(d.tojson(), 'ISODate("0NaN-NaN-NaNTNaN:NaN:NaNZ")');

        d = Date.prototype;
        assert.eq(d.tojson(), 'ISODate("0NaN-NaN-NaNTNaN:NaN:NaNZ")');
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

        describe("edge cases", function() {
            it("SERVER-8164: ISODate doesn't handle years less than 100 properly.", function() {
                [["0000-01-01", "0000-01-01T00:00:00Z"],
                 ["0000-01-01T00:00:00", "0000-01-01T00:00:00Z"],
                 ["0000-01-01T00:00:00Z", "0000-01-01T00:00:00Z"],
                 ["0000-01-01T00:00:00.123", "0000-01-01T00:00:00.123Z"],
                 ["0000-01-01T00:00:00.123Z", "0000-01-01T00:00:00.123Z"],
                 ["0000-01-01T00:00:00.1Z", "0000-01-01T00:00:00.100Z"],
                 ["0000-01-01T00:00:00.10Z", "0000-01-01T00:00:00.100Z"],
                 ["0000-01-01T00:00:00.100Z", "0000-01-01T00:00:00.100Z"],
                 ["0000-01-01T00:00:00.1000Z", "0000-01-01T00:00:00.100Z"],
                 ["0000-01-01T00:00:00.1234Z", "0000-01-01T00:00:00.123Z"],
                 ["0000-01-01T00:00:00.1235Z", "0000-01-01T00:00:00.124Z"],
                ].forEach(([input, output]) => {
                    assert.eq(ISODate(input).tojson(), `ISODate("${output}")`, input);
                });
            });
            it("Testing different years", function() {
                [["0000-01-01T00:00:00Z", "0000-01-01T00:00:00Z"],
                 ["0001-01-01T00:00:00Z", "0001-01-01T00:00:00Z"],
                 ["0069-01-01T00:00:00Z", "0069-01-01T00:00:00Z"],
                 ["0070-01-01T00:00:00Z", "0070-01-01T00:00:00Z"],
                 ["0099-01-01T00:00:00Z", "0099-01-01T00:00:00Z"],
                 ["0100-01-01T00:00:00Z", "0100-01-01T00:00:00Z"],
                 ["1800-01-01T00:00:00Z", "1800-01-01T00:00:00Z"],
                 ["1801-01-01T00:00:00Z", "1801-01-01T00:00:00Z"],
                 ["1869-01-01T00:00:00Z", "1869-01-01T00:00:00Z"],
                 ["1870-01-01T00:00:00Z", "1870-01-01T00:00:00Z"],
                 ["1899-01-01T00:00:00Z", "1899-01-01T00:00:00Z"],
                 ["1900-01-01T00:00:00Z", "1900-01-01T00:00:00Z"],
                 ["1901-01-01T00:00:00Z", "1901-01-01T00:00:00Z"],
                 ["1969-01-01T00:00:00Z", "1969-01-01T00:00:00Z"],
                 ["1970-01-01T00:00:00Z", "1970-01-01T00:00:00Z"],
                 ["1999-01-01T00:00:00Z", "1999-01-01T00:00:00Z"],
                 ["2000-01-01T00:00:00Z", "2000-01-01T00:00:00Z"],
                 ["2001-01-01T00:00:00Z", "2001-01-01T00:00:00Z"],
                 ["2069-01-01T00:00:00Z", "2069-01-01T00:00:00Z"],
                 ["2070-01-01T00:00:00Z", "2070-01-01T00:00:00Z"],
                 ["2099-01-01T00:00:00Z", "2099-01-01T00:00:00Z"],
                 ["9999-01-01T00:00:00Z", "9999-01-01T00:00:00Z"],
                ].forEach(([input, output]) => {
                    assert.eq(ISODate(input).tojson(), `ISODate("${output}")`, input);
                });
            });

            it("Testing without - in date and : in time", function() {
                [["19980101T00:00:00Z", "1998-01-01T00:00:00Z"],
                 ["1999-0101T00:00:00Z", "1999-01-01T00:00:00Z"],
                 ["200001-01T00:00:00Z", "2000-01-01T00:00:00Z"],
                 ["1998-01-01T000000Z", "1998-01-01T00:00:00Z"],
                 ["1999-01-01T00:0000Z", "1999-01-01T00:00:00Z"],
                 ["2000-01-01T0000:00Z", "2000-01-01T00:00:00Z"],
                ].forEach(([input, output]) => {
                    assert.eq(ISODate(input).tojson(), `ISODate("${output}")`, input);
                });
            });

            it("Testing field overflows", function() {
                [["0000-01-01T00:00:60Z", "0000-01-01T00:01:00Z"],
                 ["0000-01-01T00:00:99Z", "0000-01-01T00:01:39Z"],
                 ["0000-01-01T00:60:00Z", "0000-01-01T01:00:00Z"],
                 ["0000-01-01T00:99:00Z", "0000-01-01T01:39:00Z"],
                 ["0000-01-01T24:00:00Z", "0000-01-02T00:00:00Z"],
                 ["0000-01-01T99:00:00Z", "0000-01-05T03:00:00Z"],
                 ["0000-01-32T00:00:00Z", "0000-02-01T00:00:00Z"],
                 ["0000-01-99T00:00:00Z", "0000-04-08T00:00:00Z"],
                 ["0000-02-29T00:00:00Z", "0000-02-29T00:00:00Z"],
                 ["0000-02-30T00:00:00Z", "0000-03-01T00:00:00Z"],
                 ["0000-02-31T00:00:00Z", "0000-03-02T00:00:00Z"],
                 ["0000-02-99T00:00:00Z", "0000-05-09T00:00:00Z"],
                 ["0001-02-29T00:00:00Z", "0001-03-01T00:00:00Z"],
                 ["0001-02-30T00:00:00Z", "0001-03-02T00:00:00Z"],
                 ["0001-02-31T00:00:00Z", "0001-03-03T00:00:00Z"],
                 ["0001-02-99T00:00:00Z", "0001-05-10T00:00:00Z"],
                 ["0000-13-01T00:00:00Z", "0001-01-01T00:00:00Z"],
                 ["0000-99-01T00:00:00Z", "0008-03-01T00:00:00Z"],
                ].forEach(([input, output]) => {
                    assert.eq(ISODate(input).tojson(), `ISODate("${output}")`, input);
                });
            });

            it("Testing GMT offset instead of Z", function() {
                [["0001-01-01T00:00:00+01", "0000-12-31T23:00:00Z"],
                 ["0001-01-01T00:00:00+99", "0000-12-27T21:00:00Z"],
                 ["0001-01-01T00:00:00-01", "0001-01-01T01:00:00Z"],
                 ["0001-01-01T00:00:00-99", "0001-01-05T03:00:00Z"],
                 ["0001-01-01T00:00:00+0100", "0000-12-31T23:00:00Z"],
                 ["0001-01-01T00:00:00+0160", "0000-12-31T22:00:00Z"],
                 ["0001-01-01T00:00:00+0199", "0000-12-31T21:21:00Z"],
                 ["0001-01-01T00:00:00+9999", "0000-12-27T19:21:00Z"],
                 ["0001-01-01T00:00:00-0100", "0001-01-01T01:00:00Z"],
                 ["0001-01-01T00:00:00-0160", "0001-01-01T02:00:00Z"],
                 ["0001-01-01T00:00:00-0199", "0001-01-01T02:39:00Z"],
                 ["0001-01-01T00:00:00-9999", "0001-01-05T04:39:00Z"],
                 ["0001-01-01T00:00:00+01:00", "0000-12-31T23:00:00Z"],
                 ["0001-01-01T00:00:00+01:60", "0000-12-31T22:00:00Z"],
                 ["0001-01-01T00:00:00+01:99", "0000-12-31T21:21:00Z"],
                 ["0001-01-01T00:00:00+99:99", "0000-12-27T19:21:00Z"],
                 ["0001-01-01T00:00:00-01:00", "0001-01-01T01:00:00Z"],
                 ["0001-01-01T00:00:00-01:60", "0001-01-01T02:00:00Z"],
                 ["0001-01-01T00:00:00-01:99", "0001-01-01T02:39:00Z"],
                 ["0001-01-01T00:00:00-99:99", "0001-01-05T04:39:00Z"],
                ].forEach(([input, output]) => {
                    assert.eq(ISODate(input).tojson(), `ISODate("${output}")`, input);
                });
            });

            it("Testing field underflows", function() {
                [["0001-01-00T00:00:00Z", "0000-12-31T00:00:00Z"],
                 ["0001-00-00T00:00:00Z", "0000-11-30T00:00:00Z"],
                 ["0001-00-01T00:00:00Z", "0000-12-01T00:00:00Z"],
                ].forEach(([input, output]) => {
                    assert.eq(ISODate(input).tojson(), `ISODate("${output}")`, input);
                });
            });
            it("Testing lowest and highest", function() {
                [["0000-01-01T00:00:00Z", "0000-01-01T00:00:00Z"],
                 ["9999-12-31T23:59:59.999Z", "9999-12-31T23:59:59.999Z"],
                ].forEach(([input, output]) => {
                    assert.eq(ISODate(input).tojson(), `ISODate("${output}")`, input);
                });
            });
            it("Testing out of range", function() {
                assert.throws(() => ISODate("0000-01-00T23:59:59.999Z"));
                assert.throws(() => ISODate("9999-12-31T23:59:60Z"));
            });

            it("Testing broken format", function() {
                ["2017",
                 "2017-09",
                 "2017-09-16T18:37 25Z",
                 "2017-09-16T18 37:25Z",
                 "2017-09-16X18:37:25Z",
                 "2017-09 16T18:37:25Z",
                 "2017 09-16T18:37:25Z",
                 "2017-09-16T18:37:25 123Z",
                 "2017-09-16T18:37:25 0600",
                ].forEach(datestr => {
                    let e = assert.throws(() => ISODate(datestr), [datestr]);
                    assert.eq(e.message.startsWith("invalid ISO date"), true, e.message);
                });
            });

            it("Testing conversion to milliseconds", function() {
                assert.eq(ISODate("1969-12-31T23:59:59.999Z"), new Date(-1));
                assert.eq(ISODate("1969-12-31T23:59:59.000Z"), new Date(-1000));
                assert.eq(ISODate("1900-01-01T00:00:00.000Z"), new Date(-2208988800000));
                assert.eq(ISODate("1899-12-31T23:59:59.999Z"), new Date(-2208988800001));
                assert.eq(ISODate("0000-01-01T00:00:00.000Z"), new Date(-62167219200000));
                assert.eq(ISODate("9999-12-31T23:59:59.999Z"), new Date(253402300799999));
            });
        });
    });
});
