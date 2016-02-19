/*
  Run all the aggregation tests
*/

/* load the test documents */
load('jstests/aggregation/data/isodates.js');

// make sure we're using the right db; this is the same as "use mydb;" in shell
db = db.getSiblingDB("aggdb");


var i1 = db.runCommand(
{ aggregate : "isodates", pipeline : [
    { $project : {
        isoWeek: { $isoWeek: "$begin" },
        isoWeekYear: { $isoWeekYear: "$begin" },
        isoDayOfWeek: { $isoDayOfWeek: "$begin" }
    }}
]});


var i1result = [
    { "_id" : 2007, "isoWeek" : 1,  "isoWeekYear" : 2007, "isoDayOfWeek" : 1 },
    { "_id" : 1996, "isoWeek" : 1,  "isoWeekYear" : 1996, "isoDayOfWeek" : 1 },
    { "_id" : 2008, "isoWeek" : 1,  "isoWeekYear" : 2008, "isoDayOfWeek" : 2 },
    { "_id" : 2013, "isoWeek" : 1,  "isoWeekYear" : 2013, "isoDayOfWeek" : 2 },
    { "_id" : 2014, "isoWeek" : 1,  "isoWeekYear" : 2014, "isoDayOfWeek" : 3 },
    { "_id" : 1992, "isoWeek" : 1,  "isoWeekYear" : 1992, "isoDayOfWeek" : 3 },
    { "_id" : 2015, "isoWeek" : 1,  "isoWeekYear" : 2015, "isoDayOfWeek" : 4 },
    { "_id" : 2004, "isoWeek" : 1,  "isoWeekYear" : 2004, "isoDayOfWeek" : 4 },
    { "_id" : 2010, "isoWeek" : 53, "isoWeekYear" : 2009, "isoDayOfWeek" : 5 },
    { "_id" : 2016, "isoWeek" : 53, "isoWeekYear" : 2015, "isoDayOfWeek" : 5 },
    { "_id" : 2011, "isoWeek" : 52, "isoWeekYear" : 2010, "isoDayOfWeek" : 6 },
    { "_id" : 2000, "isoWeek" : 52, "isoWeekYear" : 1999, "isoDayOfWeek" : 6 },
    { "_id" : 2006, "isoWeek" : 52, "isoWeekYear" : 2005, "isoDayOfWeek" : 7 },
    { "_id" : 2012, "isoWeek" : 52, "isoWeekYear" : 2011, "isoDayOfWeek" : 7 }
]

assert.docEq(i1.result, i1result, 'i1 failed');


var i2 = db.runCommand(
{ aggregate : "isodates", pipeline : [
    { $project : {
        isoWeek: { $isoWeek: "$firstChange.end" },
        isoWeekYear: { $isoWeekYear: "$firstChange.end" },
        isoDayOfWeek: { $isoDayOfWeek: "$firstChange.end" }
    }}
]});

var i2result = [
    { "_id" : 2007, "isoWeek" : 1,  "isoWeekYear" : 2007, "isoDayOfWeek" : 7 },
    { "_id" : 1996, "isoWeek" : 1,  "isoWeekYear" : 1996, "isoDayOfWeek" : 7 },
    { "_id" : 2008, "isoWeek" : 1,  "isoWeekYear" : 2008, "isoDayOfWeek" : 7 },
    { "_id" : 2013, "isoWeek" : 1,  "isoWeekYear" : 2013, "isoDayOfWeek" : 7 },
    { "_id" : 2014, "isoWeek" : 1,  "isoWeekYear" : 2014, "isoDayOfWeek" : 7 },
    { "_id" : 1992, "isoWeek" : 1,  "isoWeekYear" : 1992, "isoDayOfWeek" : 7 },
    { "_id" : 2015, "isoWeek" : 1,  "isoWeekYear" : 2015, "isoDayOfWeek" : 7 },
    { "_id" : 2004, "isoWeek" : 1,  "isoWeekYear" : 2004, "isoDayOfWeek" : 7 },
    { "_id" : 2010, "isoWeek" : 53, "isoWeekYear" : 2009, "isoDayOfWeek" : 7 },
    { "_id" : 2016, "isoWeek" : 53, "isoWeekYear" : 2015, "isoDayOfWeek" : 7 },
    { "_id" : 2011, "isoWeek" : 52, "isoWeekYear" : 2010, "isoDayOfWeek" : 7 },
    { "_id" : 2000, "isoWeek" : 52, "isoWeekYear" : 1999, "isoDayOfWeek" : 7 },
    { "_id" : 2006, "isoWeek" : 52, "isoWeekYear" : 2005, "isoDayOfWeek" : 7 },
    { "_id" : 2012, "isoWeek" : 52, "isoWeekYear" : 2011, "isoDayOfWeek" : 7 }
]

assert.docEq(i2.result, i2result, 'i2 failed');


var i3 = db.runCommand(
{ aggregate : "isodates", pipeline : [
    { $project : {
        isoWeek: { $isoWeek: "$firstChange.next" },
        isoWeekYear: { $isoWeekYear: "$firstChange.next" },
        isoDayOfWeek: { $isoDayOfWeek: "$firstChange.next" }
    }}
]});

var i3result = [
    { "_id" : 2007, "isoWeek" : 2, "isoWeekYear" : 2007, "isoDayOfWeek" : 1 },
    { "_id" : 1996, "isoWeek" : 2, "isoWeekYear" : 1996, "isoDayOfWeek" : 1 },
    { "_id" : 2008, "isoWeek" : 2, "isoWeekYear" : 2008, "isoDayOfWeek" : 1 },
    { "_id" : 2013, "isoWeek" : 2, "isoWeekYear" : 2013, "isoDayOfWeek" : 1 },
    { "_id" : 2014, "isoWeek" : 2, "isoWeekYear" : 2014, "isoDayOfWeek" : 1 },
    { "_id" : 1992, "isoWeek" : 2, "isoWeekYear" : 1992, "isoDayOfWeek" : 1 },
    { "_id" : 2015, "isoWeek" : 2, "isoWeekYear" : 2015, "isoDayOfWeek" : 1 },
    { "_id" : 2004, "isoWeek" : 2, "isoWeekYear" : 2004, "isoDayOfWeek" : 1 },
    { "_id" : 2010, "isoWeek" : 1, "isoWeekYear" : 2010, "isoDayOfWeek" : 1 },
    { "_id" : 2016, "isoWeek" : 1, "isoWeekYear" : 2016, "isoDayOfWeek" : 1 },
    { "_id" : 2011, "isoWeek" : 1, "isoWeekYear" : 2011, "isoDayOfWeek" : 1 },
    { "_id" : 2000, "isoWeek" : 1, "isoWeekYear" : 2000, "isoDayOfWeek" : 1 },
    { "_id" : 2006, "isoWeek" : 1, "isoWeekYear" : 2006, "isoDayOfWeek" : 1 },
    { "_id" : 2012, "isoWeek" : 1, "isoWeekYear" : 2012, "isoDayOfWeek" : 1 }
]

assert.docEq(i3.result, i3result, 'i3 failed');


var i4 = db.runCommand(
{ aggregate : "isodates", pipeline : [
    { $project : {
        isoWeek: { $isoWeek: "$end" },
        isoWeekYear: { $isoWeekYear: "$end" },
        isoDayOfWeek: { $isoDayOfWeek: "$end" }
    }}
]});


var i4result = [
    { "_id" : 2007, "isoWeek" : 1,  "isoWeekYear" : 2008, "isoDayOfWeek" : 1 },
    { "_id" : 1996, "isoWeek" : 1,  "isoWeekYear" : 1997, "isoDayOfWeek" : 2 },
    { "_id" : 2008, "isoWeek" : 1,  "isoWeekYear" : 2009, "isoDayOfWeek" : 3 },
    { "_id" : 2013, "isoWeek" : 1,  "isoWeekYear" : 2014, "isoDayOfWeek" : 2 },
    { "_id" : 2014, "isoWeek" : 1,  "isoWeekYear" : 2015, "isoDayOfWeek" : 3 },
    { "_id" : 1992, "isoWeek" : 53, "isoWeekYear" : 1992, "isoDayOfWeek" : 4 },
    { "_id" : 2015, "isoWeek" : 53, "isoWeekYear" : 2015, "isoDayOfWeek" : 4 },
    { "_id" : 2004, "isoWeek" : 53, "isoWeekYear" : 2004, "isoDayOfWeek" : 5 },
    { "_id" : 2010, "isoWeek" : 52, "isoWeekYear" : 2010, "isoDayOfWeek" : 5 },
    { "_id" : 2016, "isoWeek" : 52, "isoWeekYear" : 2016, "isoDayOfWeek" : 6 },
    { "_id" : 2011, "isoWeek" : 52, "isoWeekYear" : 2011, "isoDayOfWeek" : 6 },
    { "_id" : 2000, "isoWeek" : 52, "isoWeekYear" : 2000, "isoDayOfWeek" : 7 },
    { "_id" : 2006, "isoWeek" : 52, "isoWeekYear" : 2006, "isoDayOfWeek" : 7 },
    { "_id" : 2012, "isoWeek" : 1,  "isoWeekYear" : 2013, "isoDayOfWeek" : 1 }
]

assert.docEq(i4.result, i4result, 'i4 failed');


var i5 = db.runCommand(
{ aggregate : "isodates", pipeline : [
    { $project : {
        isoWeek: { $isoWeek: "$lastChange.begin" },
        isoWeekYear: { $isoWeekYear: "$lastChange.begin" },
        isoDayOfWeek: { $isoDayOfWeek: "$lastChange.begin" }
    }}
]});

var i5result = [
    { "_id" : 2007, "isoWeek" : 1,  "isoWeekYear" : 2008, "isoDayOfWeek" : 1 },
    { "_id" : 1996, "isoWeek" : 1,  "isoWeekYear" : 1997, "isoDayOfWeek" : 1 },
    { "_id" : 2008, "isoWeek" : 1,  "isoWeekYear" : 2009, "isoDayOfWeek" : 1 },
    { "_id" : 2013, "isoWeek" : 1,  "isoWeekYear" : 2014, "isoDayOfWeek" : 1 },
    { "_id" : 2014, "isoWeek" : 1,  "isoWeekYear" : 2015, "isoDayOfWeek" : 1 },
    { "_id" : 1992, "isoWeek" : 53, "isoWeekYear" : 1992, "isoDayOfWeek" : 1 },
    { "_id" : 2015, "isoWeek" : 53, "isoWeekYear" : 2015, "isoDayOfWeek" : 1 },
    { "_id" : 2004, "isoWeek" : 53, "isoWeekYear" : 2004, "isoDayOfWeek" : 1 },
    { "_id" : 2010, "isoWeek" : 52, "isoWeekYear" : 2010, "isoDayOfWeek" : 1 },
    { "_id" : 2016, "isoWeek" : 52, "isoWeekYear" : 2016, "isoDayOfWeek" : 1 },
    { "_id" : 2011, "isoWeek" : 52, "isoWeekYear" : 2011, "isoDayOfWeek" : 1 },
    { "_id" : 2000, "isoWeek" : 52, "isoWeekYear" : 2000, "isoDayOfWeek" : 1 },
    { "_id" : 2006, "isoWeek" : 52, "isoWeekYear" : 2006, "isoDayOfWeek" : 1 },
    { "_id" : 2012, "isoWeek" : 1,  "isoWeekYear" : 2013, "isoDayOfWeek" : 1 }
]

assert.docEq(i5.result, i5result, 'i4 failed');


var i6 = db.runCommand(
{ aggregate : "isodates", pipeline : [
    { $project : {
        isoWeek: { $isoWeek: "$lastChange.prev" },
        isoWeekYear: { $isoWeekYear: "$lastChange.prev" },
        isoDayOfWeek: { $isoDayOfWeek: "$lastChange.prev" }
    }}
]});

var i6result = [
    { "_id" : 2007, "isoWeek" : 52, "isoWeekYear" : 2007, "isoDayOfWeek" : 7 },
    { "_id" : 1996, "isoWeek" : 52, "isoWeekYear" : 1996, "isoDayOfWeek" : 7 },
    { "_id" : 2008, "isoWeek" : 52, "isoWeekYear" : 2008, "isoDayOfWeek" : 7 },
    { "_id" : 2013, "isoWeek" : 52, "isoWeekYear" : 2013, "isoDayOfWeek" : 7 },
    { "_id" : 2014, "isoWeek" : 52, "isoWeekYear" : 2014, "isoDayOfWeek" : 7 },
    { "_id" : 1992, "isoWeek" : 52, "isoWeekYear" : 1992, "isoDayOfWeek" : 7 },
    { "_id" : 2015, "isoWeek" : 52, "isoWeekYear" : 2015, "isoDayOfWeek" : 7 },
    { "_id" : 2004, "isoWeek" : 52, "isoWeekYear" : 2004, "isoDayOfWeek" : 7 },
    { "_id" : 2010, "isoWeek" : 51, "isoWeekYear" : 2010, "isoDayOfWeek" : 7 },
    { "_id" : 2016, "isoWeek" : 51, "isoWeekYear" : 2016, "isoDayOfWeek" : 7 },
    { "_id" : 2011, "isoWeek" : 51, "isoWeekYear" : 2011, "isoDayOfWeek" : 7 },
    { "_id" : 2000, "isoWeek" : 51, "isoWeekYear" : 2000, "isoDayOfWeek" : 7 },
    { "_id" : 2006, "isoWeek" : 51, "isoWeekYear" : 2006, "isoDayOfWeek" : 7 },
    { "_id" : 2012, "isoWeek" : 52, "isoWeekYear" : 2012, "isoDayOfWeek" : 7 }
]

assert.docEq(i6.result, i6result, 'i6 failed');



var i7 = db.runCommand(
{ aggregate : "isodates", pipeline : [
    { $project : {
        isoWeek: { $isoWeek: "$bday" },
        isoWeekYear: { $isoWeekYear: "$bday" },
        isoDayOfWeek: { $isoDayOfWeek: "$bday" }
    }}
]});

var i7result = [
    { "_id" : 2007, "isoWeek" : 27, "isoWeekYear" : 2007, "isoDayOfWeek" : 4 },
    { "_id" : 1996, "isoWeek" : 27, "isoWeekYear" : 1996, "isoDayOfWeek" : 5 },
    { "_id" : 2008, "isoWeek" : 27, "isoWeekYear" : 2008, "isoDayOfWeek" : 6 },
    { "_id" : 2013, "isoWeek" : 27, "isoWeekYear" : 2013, "isoDayOfWeek" : 5 },
    { "_id" : 2014, "isoWeek" : 27, "isoWeekYear" : 2014, "isoDayOfWeek" : 6 },
    { "_id" : 1992, "isoWeek" : 27, "isoWeekYear" : 1992, "isoDayOfWeek" : 7 },
    { "_id" : 2015, "isoWeek" : 27, "isoWeekYear" : 2015, "isoDayOfWeek" : 7 },
    { "_id" : 2004, "isoWeek" : 28, "isoWeekYear" : 2004, "isoDayOfWeek" : 1 },
    { "_id" : 2010, "isoWeek" : 27, "isoWeekYear" : 2010, "isoDayOfWeek" : 1 },
    { "_id" : 2016, "isoWeek" : 27, "isoWeekYear" : 2016, "isoDayOfWeek" : 2 },
    { "_id" : 2011, "isoWeek" : 27, "isoWeekYear" : 2011, "isoDayOfWeek" : 2 },
    { "_id" : 2000, "isoWeek" : 27, "isoWeekYear" : 2000, "isoDayOfWeek" : 3 },
    { "_id" : 2006, "isoWeek" : 27, "isoWeekYear" : 2006, "isoDayOfWeek" : 3 },
    { "_id" : 2012, "isoWeek" : 27, "isoWeekYear" : 2012, "isoDayOfWeek" : 4 }
]

assert.docEq(i7.result, i7result, 'i7 failed');


var i8 = db.runCommand(
{ aggregate : "isodates", pipeline : [
    { $project : {
        isoBday:  { $dateToString: { format: "%G-W%V-%u", date: "$bday"  } },
        isoBegin: { $dateToString: { format: "%G-W%V-%u", date: "$begin" } }
    }}
]});

var i8result = [
    { "_id" : 2007, "isoBday" : "2007-W27-4", "isoBegin" : "2007-W01-1" },
    { "_id" : 1996, "isoBday" : "1996-W27-5", "isoBegin" : "1996-W01-1" },
    { "_id" : 2008, "isoBday" : "2008-W27-6", "isoBegin" : "2008-W01-2" },
    { "_id" : 2013, "isoBday" : "2013-W27-5", "isoBegin" : "2013-W01-2" },
    { "_id" : 2014, "isoBday" : "2014-W27-6", "isoBegin" : "2014-W01-3" },
    { "_id" : 1992, "isoBday" : "1992-W27-7", "isoBegin" : "1992-W01-3" },
    { "_id" : 2015, "isoBday" : "2015-W27-7", "isoBegin" : "2015-W01-4" },
    { "_id" : 2004, "isoBday" : "2004-W28-1", "isoBegin" : "2004-W01-4" },
    { "_id" : 2010, "isoBday" : "2010-W27-1", "isoBegin" : "2009-W53-5" },
    { "_id" : 2016, "isoBday" : "2016-W27-2", "isoBegin" : "2015-W53-5" },
    { "_id" : 2011, "isoBday" : "2011-W27-2", "isoBegin" : "2010-W52-6" },
    { "_id" : 2000, "isoBday" : "2000-W27-3", "isoBegin" : "1999-W52-6" },
    { "_id" : 2006, "isoBday" : "2006-W27-3", "isoBegin" : "2005-W52-7" },
    { "_id" : 2012, "isoBday" : "2012-W27-4", "isoBegin" : "2011-W52-7" }
]

assert.docEq(i8.result, i8result, 'i8 failed');
