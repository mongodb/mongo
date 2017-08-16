// SERVER-8164: ISODate doesn't handle years less than 100 properly.
(function() {
    assert.eq(tojson(ISODate("0000-01-01")), 'ISODate("0000-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("0000-01-01T00:00:00")), 'ISODate("0000-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("0000-01-01T00:00:00Z")), 'ISODate("0000-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("0000-01-01T00:00:00.123")), 'ISODate("0000-01-01T00:00:00.123Z")');
    assert.eq(tojson(ISODate("0000-01-01T00:00:00.123Z")), 'ISODate("0000-01-01T00:00:00.123Z")');

    assert.eq(tojson(ISODate("0000-01-01T00:00:00.1Z")), 'ISODate("0000-01-01T00:00:00.100Z")');
    assert.eq(tojson(ISODate("0000-01-01T00:00:00.10Z")), 'ISODate("0000-01-01T00:00:00.100Z")');
    assert.eq(tojson(ISODate("0000-01-01T00:00:00.100Z")), 'ISODate("0000-01-01T00:00:00.100Z")');
    assert.eq(tojson(ISODate("0000-01-01T00:00:00.1000Z")), 'ISODate("0000-01-01T00:00:00.100Z")');

    assert.eq(tojson(ISODate("0000-01-01T00:00:00.1234Z")), 'ISODate("0000-01-01T00:00:00.123Z")');
    assert.eq(tojson(ISODate("0000-01-01T00:00:00.1235Z")), 'ISODate("0000-01-01T00:00:00.124Z")');

    /* Testing different years */
    assert.eq(tojson(ISODate("0000-01-01T00:00:00Z")), 'ISODate("0000-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00Z")), 'ISODate("0001-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("0069-01-01T00:00:00Z")), 'ISODate("0069-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("0070-01-01T00:00:00Z")), 'ISODate("0070-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("0099-01-01T00:00:00Z")), 'ISODate("0099-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("0100-01-01T00:00:00Z")), 'ISODate("0100-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("1800-01-01T00:00:00Z")), 'ISODate("1800-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("1801-01-01T00:00:00Z")), 'ISODate("1801-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("1869-01-01T00:00:00Z")), 'ISODate("1869-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("1870-01-01T00:00:00Z")), 'ISODate("1870-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("1899-01-01T00:00:00Z")), 'ISODate("1899-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("1900-01-01T00:00:00Z")), 'ISODate("1900-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("1901-01-01T00:00:00Z")), 'ISODate("1901-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("1969-01-01T00:00:00Z")), 'ISODate("1969-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("1970-01-01T00:00:00Z")), 'ISODate("1970-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("1999-01-01T00:00:00Z")), 'ISODate("1999-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("2000-01-01T00:00:00Z")), 'ISODate("2000-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("2001-01-01T00:00:00Z")), 'ISODate("2001-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("2069-01-01T00:00:00Z")), 'ISODate("2069-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("2070-01-01T00:00:00Z")), 'ISODate("2070-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("2099-01-01T00:00:00Z")), 'ISODate("2099-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("9999-01-01T00:00:00Z")), 'ISODate("9999-01-01T00:00:00Z")');

    /* Testing without - in date and : in time */
    assert.eq(tojson(ISODate("19980101T00:00:00Z")), 'ISODate("1998-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("1999-0101T00:00:00Z")), 'ISODate("1999-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("200001-01T00:00:00Z")), 'ISODate("2000-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("1998-01-01T000000Z")), 'ISODate("1998-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("1999-01-01T00:0000Z")), 'ISODate("1999-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("2000-01-01T0000:00Z")), 'ISODate("2000-01-01T00:00:00Z")');

    /* Testing field overflows */
    assert.eq(tojson(ISODate("0000-01-01T00:00:60Z")), 'ISODate("0000-01-01T00:01:00Z")');
    assert.eq(tojson(ISODate("0000-01-01T00:00:99Z")), 'ISODate("0000-01-01T00:01:39Z")');

    assert.eq(tojson(ISODate("0000-01-01T00:60:00Z")), 'ISODate("0000-01-01T01:00:00Z")');
    assert.eq(tojson(ISODate("0000-01-01T00:99:00Z")), 'ISODate("0000-01-01T01:39:00Z")');

    assert.eq(tojson(ISODate("0000-01-01T24:00:00Z")), 'ISODate("0000-01-02T00:00:00Z")');
    assert.eq(tojson(ISODate("0000-01-01T99:00:00Z")), 'ISODate("0000-01-05T03:00:00Z")');

    assert.eq(tojson(ISODate("0000-01-32T00:00:00Z")), 'ISODate("0000-02-01T00:00:00Z")');
    assert.eq(tojson(ISODate("0000-01-99T00:00:00Z")), 'ISODate("0000-04-08T00:00:00Z")');
    assert.eq(tojson(ISODate("0000-02-29T00:00:00Z")), 'ISODate("0000-02-29T00:00:00Z")');
    assert.eq(tojson(ISODate("0000-02-30T00:00:00Z")), 'ISODate("0000-03-01T00:00:00Z")');
    assert.eq(tojson(ISODate("0000-02-31T00:00:00Z")), 'ISODate("0000-03-02T00:00:00Z")');
    assert.eq(tojson(ISODate("0000-02-99T00:00:00Z")), 'ISODate("0000-05-09T00:00:00Z")');

    assert.eq(tojson(ISODate("0001-02-29T00:00:00Z")), 'ISODate("0001-03-01T00:00:00Z")');
    assert.eq(tojson(ISODate("0001-02-30T00:00:00Z")), 'ISODate("0001-03-02T00:00:00Z")');
    assert.eq(tojson(ISODate("0001-02-31T00:00:00Z")), 'ISODate("0001-03-03T00:00:00Z")');
    assert.eq(tojson(ISODate("0001-02-99T00:00:00Z")), 'ISODate("0001-05-10T00:00:00Z")');

    assert.eq(tojson(ISODate("0000-13-01T00:00:00Z")), 'ISODate("0001-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("0000-99-01T00:00:00Z")), 'ISODate("0008-03-01T00:00:00Z")');

    /* Testing GMT offset instead of Z */
    assert.eq(tojson(ISODate("0001-01-01T00:00:00+01")), 'ISODate("0000-12-31T23:00:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00+99")), 'ISODate("0000-12-27T21:00:00Z")');

    assert.eq(tojson(ISODate("0001-01-01T00:00:00-01")), 'ISODate("0001-01-01T01:00:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00-99")), 'ISODate("0001-01-05T03:00:00Z")');

    assert.eq(tojson(ISODate("0001-01-01T00:00:00+0100")), 'ISODate("0000-12-31T23:00:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00+0160")), 'ISODate("0000-12-31T22:00:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00+0199")), 'ISODate("0000-12-31T21:21:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00+9999")), 'ISODate("0000-12-27T19:21:00Z")');

    assert.eq(tojson(ISODate("0001-01-01T00:00:00-0100")), 'ISODate("0001-01-01T01:00:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00-0160")), 'ISODate("0001-01-01T02:00:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00-0199")), 'ISODate("0001-01-01T02:39:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00-9999")), 'ISODate("0001-01-05T04:39:00Z")');

    assert.eq(tojson(ISODate("0001-01-01T00:00:00+01:00")), 'ISODate("0000-12-31T23:00:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00+01:60")), 'ISODate("0000-12-31T22:00:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00+01:99")), 'ISODate("0000-12-31T21:21:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00+99:99")), 'ISODate("0000-12-27T19:21:00Z")');

    assert.eq(tojson(ISODate("0001-01-01T00:00:00-01:00")), 'ISODate("0001-01-01T01:00:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00-01:60")), 'ISODate("0001-01-01T02:00:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00-01:99")), 'ISODate("0001-01-01T02:39:00Z")');
    assert.eq(tojson(ISODate("0001-01-01T00:00:00-99:99")), 'ISODate("0001-01-05T04:39:00Z")');

    /* Testing field underflows */
    assert.eq(tojson(ISODate("0001-01-00T00:00:00Z")), 'ISODate("0000-12-31T00:00:00Z")');
    assert.eq(tojson(ISODate("0001-00-00T00:00:00Z")), 'ISODate("0000-11-30T00:00:00Z")');
    assert.eq(tojson(ISODate("0001-00-01T00:00:00Z")), 'ISODate("0000-12-01T00:00:00Z")');

    /* Testing lowest and highest */
    assert.eq(tojson(ISODate("0000-01-01T00:00:00Z")), 'ISODate("0000-01-01T00:00:00Z")');
    assert.eq(tojson(ISODate("9999-12-31T23:59:59.999Z")), 'ISODate("9999-12-31T23:59:59.999Z")');

    /* Testing out of range */
    assert.throws(function() {
        tojson(ISODate("0000-01-00T23:59:59.999Z"));
    });
    assert.throws(function() {
        tojson(ISODate("9999-12-31T23:59:60Z"));
    });

    /* Testing broken format */
    var brokenFormatTests = [
        "2017",
        "2017-09",
        "2017-09-16T18:37 25Z",
        "2017-09-16T18 37:25Z",
        "2017-09-16X18:37:25Z",
        "2017-09 16T18:37:25Z",
        "2017 09-16T18:37:25Z",
        "2017-09-16T18:37:25 123Z",
        "2017-09-16T18:37:25 0600",
    ];

    brokenFormatTests.forEach(function(test) {
        assert.throws(function() {
            print(tojson(ISODate(test)));
        }, [tojson(test)]);
    });

    /* Testing conversion to milliseconds */
    assert.eq(ISODate("1969-12-31T23:59:59.999Z"), new Date(-1));
    assert.eq(ISODate("1969-12-31T23:59:59.000Z"), new Date(-1000));
    assert.eq(ISODate("1900-01-01T00:00:00.000Z"), new Date(-2208988800000));
    assert.eq(ISODate("1899-12-31T23:59:59.999Z"), new Date(-2208988800001));
    assert.eq(ISODate("0000-01-01T00:00:00.000Z"), new Date(-62167219200000));
    assert.eq(ISODate("9999-12-31T23:59:59.999Z"), new Date(253402300799999));
}());
