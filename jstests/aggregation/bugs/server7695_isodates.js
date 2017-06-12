// SERVER-7695: Add $isoWeek, $isoWeekYear, and $isoDayOfWeek aggregation expressions.

(function() {
    "use strict";
    const coll = db.server7695;
    let testOpCount = 0;

    load('jstests/libs/dateutil.js');

    coll.drop();

    // Seed collection so that the pipeline will execute.
    assert.writeOK(coll.insert({}));

    /**
     * Helper for testing that 'op' returns 'expResult'.
     */
    function testOp(op, value, expResult) {
        testOpCount++;
        let pipeline = [{$project: {_id: 0, result: {}}}];
        pipeline[0].$project.result[op] = value;
        let msg = "Exptected {" + op + ": " + value + "} to equal: " + expResult;
        let res = coll.runCommand('aggregate', {pipeline: pipeline, cursor: {}});

        // in the case of $dateToString the date is on property date
        let date = value.date || value;
        if (date.valueOf() < 0 && _isWindows() && res.code === 16422) {
            // some versions of windows (but not all) fail with dates before 1970
            print("skipping test of " + date.tojson() +
                  " because system doesn't support old dates");
            return;
        }

        if (date.valueOf() / 1000 < -2 * 1024 * 1024 * 1024 && res.code == 16421) {
            // we correctly detected that we are outside of the range of a 32-bit time_t
            print("skipping test of " + date.tojson() + " because it is outside of time_t range");
            return;
        }

        assert.eq(res.cursor.firstBatch[0].result, expResult, tojson(pipeline));
    }

    // While development, there was a bug which caused an error with $dateToString if the order of
    // %V and %G changed, so I added this test to prevent regression.
    testOp('$dateToString', {date: new Date("1900-12-31T23:59:59Z"), format: "%V-%G"}, "01-1901");
    // This was failing, but it shouldn't as it is the same as above, only rotated.
    testOp('$dateToString', {date: new Date("1900-12-31T23:59:59Z"), format: "%G-%V"}, "1901-01");

    // 1900 is special because it's devisible by 4 and by 100 but not 400 so it's not a leap year.
    // 2000 is special, because it's devisible by 4, 100, 400 and so it is a leap year.
    const years = {
        common: [
            1900,  // Starting and ending on Monday (special).
            2002,  // Starting and ending on Tuesday.
            2014,  // Starting and ending on Wednesday.
            2015,  // Starting and ending on Thursday.
            2010,  // Starting and ending on Friday.
            2011,  // Starting and ending on Saturday.
            2006,  // Starting and ending on Sunday.
        ],
        leap: [
            1996,  // Starting on Monday, ending on Tuesday.
            2008,  // Starting on Tuesday, ending on Wednesday.
            1992,  // Starting on Wednesday, ending on Thursday.
            2004,  // Starting on Thursday, ending on Friday.
            2016,  // Starting on Friday, ending on Saturday.
            2000,  // Starting on Saturday, ending on Sunday (special).
            2012,  // Starting on Sunday, ending on Monday.
        ],
        commonAfterLeap: [
            2001,  // Starting and ending on Monday.
            2013,  // Starting and ending on Tuesday.
            1997,  // Starting and ending on Wednesday.
            2009,  // Starting and ending on Thursday.
            1993,  // Starting and ending on Friday.
            2005,  // Starting and ending on Saturday.
            2017,  // Starting and ending on Sunday.
        ],
    };

    const MONDAY = 1;
    const TUESDAY = 2;
    const WEDNESDAY = 3;
    const THURSDAY = 4;
    const FRIDAY = 5;
    const SATURDAY = 6;
    const SUNDAY = 7;

    ['common', 'leap', 'commonAfterLeap'].forEach(function(type) {
        years[type].forEach(function(year, day) {
            // forEach starts indexing at zero but weekdays start with Monday on 1 so we add +1.
            day = day + 1;
            let newYear = DateUtil.getNewYear(year);
            let endOfFirstWeekInYear = DateUtil.getEndOfFirstWeekInYear(year, day);
            let startOfSecondWeekInYear = DateUtil.getStartOfSecondWeekInYear(year, day);
            let birthday = DateUtil.getBirthday(year);
            let endOfSecondToLastWeekInYear =
                DateUtil.getEndOfSecondToLastWeekInYear(year, day, type);
            let startOfLastWeekInYear = DateUtil.getStartOfLastWeekInYear(year, day, type);
            let newYearsEve = DateUtil.getNewYearsEve(year);

            testOp('$isoDayOfWeek', newYear, day);
            testOp('$isoDayOfWeek', endOfFirstWeekInYear, SUNDAY);
            testOp('$isoDayOfWeek', startOfSecondWeekInYear, MONDAY);
            testOp('$isoDayOfWeek', endOfSecondToLastWeekInYear, SUNDAY);
            testOp('$isoDayOfWeek', startOfLastWeekInYear, MONDAY);
            if (type === 'leap') {
                testOp('$isoDayOfWeek', newYearsEve, DateUtil.shiftWeekday(day, 1));
            } else {
                testOp('$isoDayOfWeek', newYearsEve, day);
            }

            if (type === 'leap') {
                testOp('$isoDayOfWeek', birthday, DateUtil.shiftWeekday(day, 4));
            } else {
                testOp('$isoDayOfWeek', birthday, DateUtil.shiftWeekday(day, 3));
            }

            testOp('$isoWeekYear', birthday, year);
            // In leap years staring on Thursday, the birthday is in week 28, every year else it is
            // in week 27.
            if (type === 'leap' && day === THURSDAY) {
                testOp('$isoWeek', birthday, 28);
            } else {
                testOp('$isoWeek', birthday, 27);
            }

            if (day <= THURSDAY) {
                // A year starting between Monday and Thursday will always start in week 1.
                testOp('$isoWeek', newYear, 1);
                testOp('$isoWeekYear', newYear, year);
                testOp('$isoWeek', endOfFirstWeekInYear, 1);
                testOp('$isoWeekYear', endOfFirstWeekInYear, year);
                testOp('$isoWeek', startOfSecondWeekInYear, 2);
                testOp('$isoWeekYear', startOfSecondWeekInYear, year);
                testOp('$dateToString',
                       {format: '%G-W%V-%u', date: newYear},
                       "" + year + "-W01-" + day);
            } else if (day == FRIDAY || (day == SATURDAY && type === 'commonAfterLeap')) {
                // A year starting on Friday will always start with week 53 of the previous year.
                // A common year starting on a Saturday and after a leap year will also start with
                // week 53 of the previous year.
                testOp('$isoWeek', newYear, 53);
                testOp('$isoWeekYear', newYear, year - 1);
                testOp('$isoWeek', endOfFirstWeekInYear, 53);
                testOp('$isoWeekYear', endOfFirstWeekInYear, year - 1);
                testOp('$isoWeek', startOfSecondWeekInYear, 1);
                testOp('$isoWeekYear', startOfSecondWeekInYear, year);
                testOp('$dateToString',
                       {format: '%G-W%V-%u', date: newYear},
                       "" + (year - 1) + "-W53-" + day);
            } else {
                // A year starting on Saturday (except after a leap year) or Sunday will always
                // start with week 52 of the previous year.
                testOp('$isoWeek', newYear, 52);
                testOp('$isoWeekYear', newYear, year - 1);
                testOp('$isoWeek', endOfFirstWeekInYear, 52);
                testOp('$isoWeekYear', endOfFirstWeekInYear, year - 1);
                testOp('$isoWeek', startOfSecondWeekInYear, 1);
                testOp('$isoWeekYear', startOfSecondWeekInYear, year);
                testOp('$dateToString',
                       {format: '%G-W%V-%u', date: newYear},
                       "" + (year - 1) + "-W52-" + day);
            }

            if (type === 'leap') {
                if (day <= TUESDAY) {
                    // A leap year starting between Monday and Tuesday will always end in week 1 of
                    // the next year.
                    testOp('$isoWeek', newYearsEve, 1);
                    testOp('$isoWeekYear', newYearsEve, year + 1);
                    testOp('$isoWeek', endOfSecondToLastWeekInYear, 52);
                    testOp('$isoWeekYear', endOfSecondToLastWeekInYear, year);
                    testOp('$isoWeek', startOfLastWeekInYear, 1);
                    testOp('$isoWeekYear', startOfLastWeekInYear, year + 1);
                    testOp('$dateToString',
                           {format: '%G-W%V-%u', date: newYearsEve},
                           "" + (year + 1) + "-W01-" + DateUtil.shiftWeekday(day, 1));
                } else if (day <= THURSDAY) {
                    // A leap year starting on Wednesday or Thursday will always end with week 53.
                    testOp('$isoWeek', newYearsEve, 53);
                    testOp('$isoWeekYear', newYearsEve, year);
                    testOp('$isoWeek', endOfSecondToLastWeekInYear, 52);
                    testOp('$isoWeekYear', endOfSecondToLastWeekInYear, year);
                    testOp('$isoWeek', startOfLastWeekInYear, 53);
                    testOp('$isoWeekYear', startOfLastWeekInYear, year);
                    testOp('$dateToString',
                           {format: '%G-W%V-%u', date: newYearsEve},
                           "" + (year) + "-W53-" + DateUtil.shiftWeekday(day, 1));
                } else if (day <= SATURDAY) {
                    // A leap year starting on Friday or Sarturday will always and with week 52
                    testOp('$isoWeek', newYearsEve, 52);
                    testOp('$isoWeekYear', newYearsEve, year);
                    testOp('$isoWeek', endOfSecondToLastWeekInYear, 51);
                    testOp('$isoWeekYear', endOfSecondToLastWeekInYear, year);
                    testOp('$isoWeek', startOfLastWeekInYear, 52);
                    testOp('$isoWeekYear', startOfLastWeekInYear, year);
                    testOp('$dateToString',
                           {format: '%G-W%V-%u', date: newYearsEve},
                           "" + (year) + "-W52-" + DateUtil.shiftWeekday(day, 1));
                } else {
                    // A leap year starting on Sunday will always end with week 1
                    testOp('$isoWeek', newYearsEve, 1);
                    testOp('$isoWeekYear', newYearsEve, year + 1);
                    testOp('$isoWeek', endOfSecondToLastWeekInYear, 51);
                    testOp('$isoWeekYear', endOfSecondToLastWeekInYear, year);
                    testOp('$isoWeek', startOfLastWeekInYear, 52);
                    testOp('$isoWeekYear', startOfLastWeekInYear, year);
                    testOp('$dateToString',
                           {format: '%G-W%V-%u', date: newYearsEve},
                           "" + (year + 1) + "-W01-" + DateUtil.shiftWeekday(day, 1));
                }
            } else {
                if (day <= WEDNESDAY) {
                    // A common year starting between Monday and Wednesday will always end in week 1
                    // of the next year.
                    testOp('$isoWeek', newYearsEve, 1);
                    testOp('$isoWeekYear', newYearsEve, year + 1);
                    testOp('$isoWeek', endOfSecondToLastWeekInYear, 52);
                    testOp('$isoWeekYear', endOfSecondToLastWeekInYear, year);
                    testOp('$isoWeek', startOfLastWeekInYear, 1);
                    testOp('$isoWeekYear', startOfLastWeekInYear, year + 1);
                    testOp('$dateToString',
                           {format: '%G-W%V-%u', date: newYearsEve},
                           "" + (year + 1) + "-W01-" + day);
                } else if (day === THURSDAY) {
                    // A common year starting on Thursday will always end with week 53.
                    testOp('$isoWeek', newYearsEve, 53);
                    testOp('$isoWeekYear', newYearsEve, year);
                    testOp('$isoWeek', endOfSecondToLastWeekInYear, 52);
                    testOp('$isoWeekYear', endOfSecondToLastWeekInYear, year);
                    testOp('$isoWeek', startOfLastWeekInYear, 53);
                    testOp('$isoWeekYear', startOfLastWeekInYear, year);
                    testOp('$dateToString',
                           {format: '%G-W%V-%u', date: newYearsEve},
                           "" + (year) + "-W53-" + day);
                } else {
                    // A common year starting on between Friday and Sunday will always end with week
                    // 52.
                    testOp('$isoWeek', newYearsEve, 52);
                    testOp('$isoWeekYear', newYearsEve, year);
                    testOp('$isoWeek', endOfSecondToLastWeekInYear, 51);
                    testOp('$isoWeekYear', endOfSecondToLastWeekInYear, year);
                    testOp('$isoWeek', startOfLastWeekInYear, 52);
                    testOp('$isoWeekYear', startOfLastWeekInYear, year);
                    testOp('$dateToString',
                           {format: '%G-W%V-%u', date: newYearsEve},
                           "" + (year) + "-W52-" + day);
                }
            }
        });
    });
    assert.eq(testOpCount, 485, 'Expected 485 tests to run');
})();
