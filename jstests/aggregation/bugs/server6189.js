// server6189 - Support date operators with dates before 1970

c = db.c;
function test(date, testSynthetics) {
    print("testing " + date);

    c.drop();
    c.save( {date: date} );

    // Can't use aggregate helper or assertErrorCode because we need to handle multiple error types
    var res = c.runCommand('aggregate', {pipeline: [
                     {$project: { _id: 0
                                , year:{ $year: '$date' }
                                , month:{ $month: '$date' }
                                , dayOfMonth:{ $dayOfMonth: '$date' }
                                , hour:{ $hour: '$date' }
                                , minute:{ $minute: '$date' }
                                , second:{ $second: '$date' }
                                , millisecond:{ $millisecond: '$date' } // server-6666

                                // $substr will call coerceToString
                                , string: {$substr: ['$date', 0,1000]}
                                }}]});

    if (date.valueOf() < 0 && _isWindows() && res.code == 16422) {
        // some versions of windows (but not all) fail with dates before 1970
        print("skipping test of " + date.tojson() + " because system doesn't support old dates");
        return;
    }

    if (date.valueOf()/1000 < -2*1024*1024*1024 && res.code == 16421) {
        // we correctly detected that we are outside of the range of a 32-bit time_t
        print("skipping test of " + date.tojson() + " because it is outside of time_t range");
        return;
    }

    assert.commandWorked(res);
    assert.eq(res.result[0], { year: date.getUTCFullYear()
                             , month: date.getUTCMonth() + 1 // jan == 1
                             , dayOfMonth: date.getUTCDate()
                             , hour: date.getUTCHours()
                             , minute: date.getUTCMinutes()
                             , second: date.getUTCSeconds()
                             , millisecond: date.getUTCMilliseconds() // server-6666
                             , string: date.tojson().slice(9,28)
                             } );

    if (testSynthetics) {
        // Tests with this set all have the same value for these fields
        res = c.aggregate( { $project:{ _id: 0
                                      , week:{ $week: '$date' }
                                      , dayOfWeek:{ $dayOfWeek: '$date' }
                                      , dayOfYear:{ $dayOfYear: '$date' }
                                      } } );

        assert.commandWorked(res);
        assert.eq(res.result[0], { week: 0
                                 , dayOfWeek: 7
                                 , dayOfYear: 2
                                 } );
    }
}


// Basic test
test(ISODate('1960-01-02 03:04:05.006Z'), true);

// Testing special rounding rules for seconds
test(ISODate('1960-01-02 03:04:04.999Z'), false); // second = 4
test(ISODate('1960-01-02 03:04:05.000Z'), true); // second = 5
test(ISODate('1960-01-02 03:04:05.001Z'), true); // second = 5
test(ISODate('1960-01-02 03:04:05.999Z'), true); // second = 5

// Test date before 1900 (negative tm_year values from gmtime)
test(ISODate('1860-01-02 03:04:05.006Z'), false);

// Test with time_t == -1 and 0
test(new Date(-1000), false);
test(new Date(0), false);

// Testing dates between 1970 and 2000
test(ISODate('1970-01-01 00:00:00.000Z'), false);
test(ISODate('1970-01-01 00:00:00.999Z'), false);
test(ISODate('1980-05-20 12:53:64.834Z'), false);
test(ISODate('1999-12-31 00:00:00.000Z'), false);
test(ISODate('1999-12-31 23:59:59.999Z'), false);

// Test date > 2000 for completeness (using now)
test(new Date(), false);
