// $week returns a date's week of the year.  Week zero is comprised of any dates before the first
// Sunday of the year.  SERVER-6190
load('jstests/aggregation/extras/utils.js');

t = db.jstests_aggregation_server6190;
t.drop();

t.save( {} );

function week( date ) {
    return t.aggregate( { $project:{ a:{ $week:date } } },
                        { $match:{ a:{ $type:16 /* Int type expected */ } } } ).result[ 0 ].a;
}

function assertWeek( expectedWeek, date ) {
    assert.eq( expectedWeek, week( date ) );
}

// Sun Jan 1 1984
assertWeek( 1, new Date( 1984, 0, 1 ) );
// Mon Jan 2 1984
assertWeek( 1, new Date( 1984, 0, 2 ) );
// Sat Jan 7 1984
assertWeek( 1, new Date( 1984, 0, 7 ) );
// Sun Jan 8 1984
assertWeek( 2, new Date( 1984, 0, 8 ) );
// Sat Feb 18 1984
assertWeek( 7, new Date( 1984, 1, 18 ) );
// Sun Feb 19 1984
assertWeek( 8, new Date( 1984, 1, 19 ) );

// Mon Jan 1 2007
assertWeek( 0, new Date( 2007, 0, 1 ) );
// Tue Jan 2 2007
assertWeek( 0, new Date( 2007, 0, 2 ) );
// Sat Jan 6 2007
assertWeek( 0, new Date( 2007, 0, 6 ) );
// Sun Jan 7 2007
assertWeek( 1, new Date( 2007, 0, 7 ) );
// Mon Jan 8 2007
assertWeek( 1, new Date( 2007, 0, 8 ) );
// Sat Jan 13 2007
assertWeek( 1, new Date( 2007, 0, 13 ) );
// Sun Jan 14 2007
assertWeek( 2, new Date( 2007, 0, 14 ) );
// Sat Mar 3 2007
assertWeek( 8, new Date( 2007, 2, 3 ) );
// Sun Mar 4 2007
assertWeek( 9, new Date( 2007, 2, 4 ) );

// Tue Jan 1 2008
assertWeek( 0, new Date( 2008, 0, 1 ) );
// Sat Jan 5 2008
assertWeek( 0, new Date( 2008, 0, 5 ) );
// Sun Jan 6 2008
assertWeek( 1, new Date( 2008, 0, 6 ) );
// Sat Apr 26 2008
assertWeek( 16, new Date( 2008, 3, 26 ) );
// Sun Apr 27 2008
assertWeek( 17, new Date( 2008, 3, 27 ) );

// Wed Jan 1 2003
assertWeek( 0, new Date( 2003, 0, 1 ) );
// Sat Jan 4 2003
assertWeek( 0, new Date( 2003, 0, 4 ) );
// Sun Jan 5 2003
assertWeek( 1, new Date( 2003, 0, 5 ) );
// Sat Dec 27 2003
assertWeek( 51, new Date( 2003, 11, 27 ) );
// Sat Dec 28 2003
assertWeek( 52, new Date( 2003, 11, 28 ) );

// Thu Jan 1 2009
assertWeek( 0, new Date( 2009, 0, 1 ) );
// Sat Jan 3 2009
assertWeek( 0, new Date( 2009, 0, 3 ) );
// Sun Jan 4 2008
assertWeek( 1, new Date( 2009, 0, 4 ) );
// Sat Oct 31 2009
assertWeek( 43, new Date( 2009, 9, 31 ) );
// Sun Nov 1 2008
assertWeek( 44, new Date( 2009, 10, 1 ) );

// Fri Jan 1 2010
assertWeek( 0, new Date( 2010, 0, 1 ) );
// Sat Jan 2 2010
assertWeek( 0, new Date( 2010, 0, 2 ) );
// Sun Jan 3 2010
assertWeek( 1, new Date( 2010, 0, 3 ) );
// Sat Sept 18 2010
assertWeek( 37, new Date( 2010, 8, 18 ) );
// Sun Sept 19 2010
assertWeek( 38, new Date( 2010, 8, 19 ) );

// Sat Jan 1 2011
assertWeek( 0, new Date( 2011, 0, 1 ) );
// Sun Jan 2 2011
assertWeek( 1, new Date( 2011, 0, 2 ) );
// Sat Aug 20 2011
assertWeek( 33, new Date( 2011, 7, 20 ) );
// Sun Aug 21 2011
assertWeek( 34, new Date( 2011, 7, 21 ) );


// Leap year tests.

// Sat Feb 27 2016
assertWeek( 8, new Date( 2016, 1, 27 ) );
// Sun Feb 28 2016
assertWeek( 9, new Date( 2016, 1, 28 ) );
// Mon Feb 29 2016
assertWeek( 9, new Date( 2016, 1, 29 ) );
// Tue Mar 1 2016
assertWeek( 9, new Date( 2016, 2, 1 ) );

// Sat Feb 28 2032
assertWeek( 8, new Date( 2032, 1, 28 ) );
// Sun Feb 29 2032
assertWeek( 9, new Date( 2032, 1, 29 ) );
// Mon Mar 1 2032
assertWeek( 9, new Date( 2032, 2, 1 ) );

// Fri Feb 28 2020
assertWeek( 8, new Date( 2020, 1, 28 ) );
// Sat Feb 29 2020
assertWeek( 8, new Date( 2020, 1, 29 ) );
// Sun Mar 1 2020
assertWeek( 9, new Date( 2020, 2, 1 ) );

// Timestamp argument.
assertWeek( 1, new Timestamp( new Date( 1984, 0, 1 ).getTime() / 1000, 0 ) );
assertWeek( 1, new Timestamp( new Date( 1984, 0, 1 ).getTime() / 1000, 1000000000 ) );

// Numeric argument not allowed.
assertErrorCode(t, {$project: {a: {$week: 5}}}, 16006);

// String argument not allowed.
assertErrorCode(t, {$project: {a: {$week: 'foo'}}}, 16006);

// Array argument format.
assertWeek( 8, [ new Date( 2016, 1, 27 ) ] );

// Wrong number of arguments.
assertErrorCode(t, {$project: {a: {$week: []}}}, 16020);
assertErrorCode(t, {$project: {a: {$week: [new Date(2020, 1, 28),
                                           new Date(2020, 1, 29)]}}},
                16020);

// From a field path expression.
t.remove();
t.save( { a:new Date( 2020, 2, 1 ) } );
assertWeek( 9, '$a' );
