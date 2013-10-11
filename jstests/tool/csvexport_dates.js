// Test that we can handle various edge cases of a date type in a csv export

t = new ToolTest("csvexport_date_before_epoch")

c = t.startDB("foo");

function test(date) {
    print("testing " + date);

    c.drop();

    assert.eq(0, c.count(), "initial collection not empty");

    c.insert({ _id : 1, date : date });

    assert.eq(1, c.count(), "failed to insert document into collection");

    t.runTool("export", "--out", t.extFile, "-d", t.baseName, "-c", "foo", "--csv", "-f",
              "_id,date")

    c.drop()

    assert.eq(0, c.count(), "failed to drop collection")

    t.runTool("import", "--file", t.extFile, "-d", t.baseName, "-c", "foo", "--type", "csv",
              "--headerline");

    assert.soon(1 + " == c.count()", "after import");

    // Note: Exporting and Importing to/from CSV is not designed to be round-trippable
    var expected = { "date" : date.toISOString() };

    var actual = c.findOne();

    delete actual._id
    assert.eq(expected, actual, "imported doc did not match expected");
}

// Basic test
test(ISODate('1960-01-02 03:04:05.006Z'));

// Testing special rounding rules for seconds
test(ISODate('1960-01-02 03:04:04.999Z')); // second = 4
test(ISODate('1960-01-02 03:04:05.000Z')); // second = 5
test(ISODate('1960-01-02 03:04:05.001Z')); // second = 5
test(ISODate('1960-01-02 03:04:05.999Z')); // second = 5

// Test date before 1900 (negative tm_year values from gmtime)
test(ISODate('1860-01-02 03:04:05.006Z'));

// Test with time_t == -1 and 0
test(new Date(-1000));
test(new Date(0));

// Testing dates between 1970 and 2000
test(ISODate('1970-01-01 00:00:00.000Z'));
test(ISODate('1970-01-01 00:00:00.999Z'));
test(ISODate('1980-05-20 12:53:64.834Z'));
test(ISODate('1999-12-31 00:00:00.000Z'));
test(ISODate('1999-12-31 23:59:59.999Z'));

// Test date > 2000 for completeness (using now)
test(new Date());

t.stop();
