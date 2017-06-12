// SERVER-9406: Allow ObjectId type to be treated as a date in date related expressions

(function() {
    "use strict";

    load('jstests/libs/dateutil.js');

    const coll = db.server9406;
    let testOpCount = 0;

    coll.drop();

    // Seed collection so that the pipeline will execute.
    assert.writeOK(coll.insert({}));

    function makeObjectIdFromDate(dt) {
        try {
            return new ObjectId((dt.getTime() / 1000).toString(16) + "f000000000000000");
        } catch (e) {
            assert("Invalid date for conversion to Object Id: " + dt);
        }
    }

    /**
     *  Helper for testing that 'op' on 'value' is the same for dates as equivalent ObjectIds
     *  'value' is either a date value, or an object containing field 'date'.
     */
    function testOp(op, value) {
        testOpCount++;

        let pipeline = [{$project: {_id: 0, result: {}}}];
        pipeline[0].$project.result[op] = value;
        let res1 = coll.aggregate(pipeline).toArray()[0];
        if (value.date) {
            value.date = makeObjectIdFromDate(value.date);
        } else {
            value = makeObjectIdFromDate(value);
        }
        pipeline[0].$project.result[op] = value;
        let res2 = coll.aggregate(pipeline).toArray()[0];

        assert.eq(res2.result, res1.result, tojson(pipeline));
    }

    testOp('$dateToString', {date: new Date("1980-12-31T23:59:59Z"), format: "%V-%G"});
    testOp('$dateToString', {date: new Date("1980-12-31T23:59:59Z"), format: "%G-%V"});

    const years = [
        2002,  // Starting and ending on Tuesday.
        2014,  // Starting and ending on Wednesday.
        2015,  // Starting and ending on Thursday.
        2010,  // Starting and ending on Friday.
        2011,  // Starting and ending on Saturday.
        2006,  // Starting and ending on Sunday.
        1996,  // Starting on Monday, ending on Tuesday.
        2008,  // Starting on Tuesday, ending on Wednesday.
        1992,  // Starting on Wednesday, ending on Thursday.
        2004,  // Starting on Thursday, ending on Friday.
        2016,  // Starting on Friday, ending on Saturday.
        2000,  // Starting on Saturday, ending on Sunday (special).
        2012   // Starting on Sunday, ending on Monday.
    ];

    const day = 1;
    years.forEach(function(year) {
        // forEach starts indexing at zero but weekdays start with Monday on 1 so we add +1.
        let newYear = DateUtil.getNewYear(year);
        let endOfFirstWeekInYear = DateUtil.getEndOfFirstWeekInYear(year, day);
        let startOfSecondWeekInYear = DateUtil.getStartOfSecondWeekInYear(year, day);
        let birthday = DateUtil.getBirthday(year);
        let newYearsEve = DateUtil.getNewYearsEve(year);
        let now = new Date();
        now.setYear(year);
        now.setMilliseconds(0);

        testOp('$isoDayOfWeek', newYear);
        testOp('$isoDayOfWeek', endOfFirstWeekInYear);
        testOp('$isoDayOfWeek', startOfSecondWeekInYear);
        testOp('$isoWeekYear', birthday);

        testOp('$isoWeek', newYear);
        testOp('$isoWeek', now);
        testOp('$isoWeekYear', newYear);
        testOp('$isoWeek', endOfFirstWeekInYear);
        testOp('$dateToString', {format: '%G-W%V-%u', date: newYear});
        testOp('$isoWeek', endOfFirstWeekInYear);
        testOp('$year', endOfFirstWeekInYear);
        testOp('$month', endOfFirstWeekInYear);
        testOp('$dayOfMonth', endOfFirstWeekInYear);
        testOp('$dayOfWeek', birthday);
        testOp('$dayOfWeek', newYearsEve);
        testOp('$minute', newYearsEve);
        testOp('$second', now);
        testOp('$millisecond', newYear);

    });
    assert.eq(testOpCount, 236, 'Expected 236 tests to run');
})();
