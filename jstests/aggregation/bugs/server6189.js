// server6189 - Support date operators with dates before 1970

c = db.c;
function test(date, testSynthetics) {
    c.drop();
    c.save( {date: date} );

    res = c.aggregate( { $project:{ _id: 0
                                , year:{ $year: '$date' }
                                , month:{ $month: '$date' }
                                , dayOfMonth:{ $dayOfMonth: '$date' }
                                , hour:{ $hour: '$date' }
                                , minute:{ $minute: '$date' }
                                , second:{ $second: '$date' }
                                //, millisecond:{ $millisecond: '$date' } // server-6666

                                // $substr will call coerceToString
                                //, string: {$substr: ['$date', 0,1000]} // server-6679
                                }} );
    assert.commandWorked(res);
    assert.eq(res.result[0], { year: date.getUTCFullYear()
                             , month: date.getUTCMonth() + 1 // jan == 1
                             , dayOfMonth: date.getUTCDate()
                             , hour: date.getUTCHours()
                             , minute: date.getUTCMinutes()
                             , second: date.getUTCSeconds()
                             //, millisecond: date.getUTCMilliseconds() // server-6666
                             //, string: date.tojson().split('"')[1] // server-6679
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

// Test date > 2000 for completeness (using now)
test(new Date(), false);
