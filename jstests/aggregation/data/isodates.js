/* sample articles for aggregation demonstrations */

// make sure we're using the right db; this is the same as "use mydb;" in shell
db = db.getSiblingDB("aggdb");
db.isodates.drop();

// Monday Common Year
db.isodates.save( {
    _id: 2007,
    begin: new Date(1167652800000),    // 2007-01-01T12:00:00Z
    firstChange: {
        end: new Date(1168171200000),  // 2007-01-07T12:00:00Z
        next: new Date(1168257600000)  // 2007-01-08T12:00:00Z
    },
    bday: new Date(1183636800000),     // 2007-07-05T12:00:00Z
    lastChange: {
        prev: new Date(1199012400000), // 2007-12-30T12:00:00Z
        begin: new Date(1199102400000) // 2007-12-31T12:00:00Z
    },
    end: new Date(1199102400000)       // 2007-12-31T12:00:00Z
});

// Monday Leap Year
db.isodates.save( {
    _id: 1996,
    begin: new Date(820497600000),    // 1996-01-01T12:00:00Z
    firstChange: {
        end: new Date(821016000000),  // 1996-01-07T12:00:00Z
        next: new Date(821102400000)  // 1996-01-08T12:00:00Z
    },
    bday: new Date(836568000000),     // 1996-07-05T12:00:00Z
    lastChange: {
        prev: new Date(851857200000), // 1996-12-29T12:00:00Z
        begin: new Date(851943600000) // 1996-12-30T12:00:00Z
    },
    end: new Date(852033600000)       // 1996-12-31T12:00:00Z
});

// Tuesday Common Year
db.isodates.save( {
    _id: 2008,
    begin: new Date(1199188800000),    // 2008-01-01T12:00:00Z
    firstChange: {
        end: new Date(1199620800000),  // 2008-01-06T12:00:00Z
        next: new Date(1199707200000)  // 2008-01-07T12:00:00Z
    },
    bday: new Date(1215259200000),     // 2008-07-05T12:00:00Z
    lastChange: {
        prev: new Date(1230462000000), // 2008-12-28T12:00:00Z
        begin: new Date(1230548400000) // 2008-12-29T12:00:00Z
    },
    end: new Date(1230724800000)       // 2008-12-31T12:00:00Z
});

// Tuesday Common Year
db.isodates.save( {
    _id: 2013,
    begin: new Date(1357041600000),    // 2013-01-01T12:00:00Z
    firstChange: {
        end: new Date(1357473600000),  // 2013-01-06T12:00:00Z
        next: new Date(1357560000000)  // 2013-01-07T12:00:00Z
    },
    bday: new Date(1373025600000),     // 2013-07-05T12:00:00Z
    lastChange: {
        prev: new Date(1388314800000), // 2013-12-29T12:00:00Z
        begin: new Date(1388401200000) // 2013-12-30T12:00:00Z
    },
    end: new Date(1388491200000)       // 2013-12-31T12:00:00Z
});

// Wednesday Common Year
db.isodates.save( {
    _id: 2014,
    begin: new Date(1388577600000),    // 2014-01-01T12:00:00Z
    firstChange: {
        end: new Date(1388923200000),  // 2014-01-05T12:00:00Z
        next: new Date(1389009600000)  // 2014-01-06T12:00:00Z
    },
    bday: new Date(1404561600000),     // 2014-07-05T12:00:00Z
    lastChange: {
        prev: new Date(1419764400000), // 2014-12-28T12:00:00Z
        begin: new Date(1419850800000) // 2014-12-29T12:00:00Z
    },
    end: new Date(1420027200000)       // 2014-12-31T12:00:00Z
});

// Wednesday Leap Year
db.isodates.save( {
    _id: 1992,
    begin: new Date(694267200000),    // 1992-01-01T12:00:00Z
    firstChange: {
        end: new Date(694612800000),  // 1992-01-05T12:00:00Z
        next: new Date(694699200000)  // 1992-01-06T12:00:00Z
    },
    bday: new Date(710337600000),     // 1992-07-05T12:00:00Z
    lastChange: {
        prev: new Date(725454000000), // 1992-12-27T12:00:00Z
        begin: new Date(725540400000) // 1992-12-28T12:00:00Z
    },
    end: new Date(725803200000)       // 1992-12-31T12:00:00Z
});

// Thursday Common Year
db.isodates.save( {
    _id: 2015,
    begin: new Date(1420113600000),    // 2015-01-01T12:00:00Z
    firstChange: {
        end: new Date(1420372800000),  // 2015-01-04T12:00:00Z
        next: new Date(1420459200000)  // 2015-01-05T12:00:00Z
    },
    bday: new Date(1436097600000),     // 2015-07-05T12:00:00Z
    lastChange: {
        prev: new Date(1451214000000), // 2015-12-27T12:00:00Z
        begin: new Date(1451300400000) // 2015-12-28T12:00:00Z
    },
    end: new Date(1451563200000)       // 2015-12-31T12:00:00Z
});

// Thursday Leap Year
db.isodates.save( {
    _id: 2004,
    begin: new Date(1072958400000),    // 2004-01-01T12:00:00Z
    firstChange: {
        end: new Date(1073217600000),  // 2004-01-04T12:00:00Z
        next: new Date(1073304000000)  // 2004-01-05T12:00:00Z
    },
    bday: new Date(1089028800000),     // 2004-07-05T12:00:00Z
    lastChange: {
        prev: new Date(1104058800000), // 2004-12-26T12:00:00Z
        begin: new Date(1104145200000) // 2004-12-27T12:00:00Z
    },
    end: new Date(1104494400000)       // 2004-12-31T12:00:00Z
});

// Friday Common Year
db.isodates.save( {
    _id: 2010,
    begin: new Date(1262347200000),    // 2010-01-01T12:00:00Z
    firstChange: {
        end: new Date(1262520000000),  // 2010-01-03T12:00:00Z
        next: new Date(1262606400000)  // 2010-01-04T12:00:00Z
    },
    bday: new Date(1278331200000),     // 2010-07-05T12:00:00Z
    lastChange: {
        prev: new Date(1293364800000), // 2010-12-26T12:00:00Z
        begin: new Date(1293447600000) // 2010-12-27T12:00:00Z
    },
    end: new Date(1293796800000)       // 2010-12-31T12:00:00Z
});

// Friday Leap Year
db.isodates.save( {
    _id: 2016,
    begin: new Date(1451649600000),    // 2016-01-01T12:00:00Z
    firstChange: {
        end: new Date(1451822400000),  // 2016-01-03T12:00:00Z
        next: new Date(1451908800000)  // 2016-01-04T12:00:00Z
    },
    bday: new Date(1467720000000),     // 2016-07-05T12:00:00Z
    lastChange: {
        prev: new Date(1482667200000), // 2016-12-25T12:00:00Z
        begin: new Date(1482753600000) // 2016-12-26T12:00:00Z
    },
    end: new Date(1483185600000)       // 2016-12-31T12:00:00Z
});

// Saturday common year
db.isodates.save( {
    _id: 2011,
    begin: new Date(1293883200000),    // 2011-01-01T12:00:00Z
    firstChange: {
        end: new Date(1293969600000),  // 2011-01-02T12:00:00Z
        next: new Date(1294056000000)  // 2011-01-03T12:00:00Z
    },
    bday: new Date(1309867200000),     // 2011-07-05T12:00:00Z
    lastChange: {
        prev: new Date(1324814400000), // 2011-12-25T12:00:00Z
        begin: new Date(1324900800000) // 2011-12-26T12:00:00Z
    },
    end: new Date(1325332800000)       // 2011-12-31T12:00:00Z
});

// Saturday Leap year
db.isodates.save( {
    _id: 2000,
    begin: new Date(946728000000),    // 2000-01-01T12:00:00Z
    firstChange: {
        end: new Date(946814400000),  // 2000-01-02T12:00:00Z
        next: new Date(946900800000)  // 2000-01-03T12:00:00Z
    },
    bday: new Date(962798400000),     // 2000-07-05T12:00:00Z
    lastChange: {
        prev: new Date(977659200000), // 2000-12-24T12:00:00Z
        begin: new Date(977745600000) // 2000-12-25T12:00:00Z
    },
    end: new Date(978264000000)       // 2000-12-31T12:00:00Z
});

// Sunday Common Year
db.isodates.save( {
    _id: 2006,
    begin: new Date(1136116800000),    // 2006-01-01T12:00:00Z
    firstChange: {
        end: new Date(1136116800000),  // 2006-01-01T12:00:00Z
        next: new Date(1136203200000)  // 2006-01-02T12:00:00Z
    },
    bday: new Date(1152100800000),     // 2006-07-05T12:00:00Z
    lastChange: {
        prev: new Date(1166961600000), // 2006-12-24T12:00:00Z
        begin: new Date(1167048000000) // 2006-12-25T12:00:00Z
    },
    end: new Date(1167566400000)       // 2006-12-31T12:00:00Z
});

// Sunday Leap Year
db.isodates.save( {
    _id: 2012,
    begin: new Date(1325419200000),    // 2012-01-01T12:00:00Z
    firstChange: {
        end: new Date(1325419200000),  // 2012-01-01T12:00:00Z
        next: new Date(1325505600000)  // 2012-01-02T12:00:00Z
    },
    bday: new Date(1341489600000),     // 2012-07-05T12:00:00Z
    lastChange: {
        prev: new Date(1356868800000), // 2012-12-30T12:00:00Z
        begin: new Date(1356955200000) // 2012-12-31T12:00:00Z
    },
    end: new Date(1356955200000)       // 2012-12-31T12:00:00Z
});
