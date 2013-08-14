// server9289 - support objects as single arguments to expressions.

var t = db.server9289;
t.drop();

t.insert({date: ISODate('2013-08-14T21:41:43Z')});

// This would result in a parse error on older servers
assert.eq(t.aggregate({$project: {year: {$year: {$add:['$date',1000]}}}}).result[0].year, 2013);
