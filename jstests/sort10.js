// signed dates check
t = db.sort2;

var opts = {};
if (Math.random() < 0.3) {
    opts.background = true;
    printjson(opts);
}
t.drop();
t.insert({ x: new Date(50000) });
t.insert({ x: new Date(-50) });
var d = new Date(-50);
for (var pass = 0; pass < 2; pass++) {
    assert(t.find().sort({x:1})[0].x.valueOf() == d.valueOf());
    t.ensureIndex({ x: 1 }, opts);
    t.insert({ x: new Date() });
}



function checkSorting(dates, sortOrder) {
    cur = t.find().sort({x:sortOrder});
    assert.eq(dates.length, cur.count(), "Incorrect number of results returned");
    index = 0;
    while (cur.hasNext()) {
        date = cur.next().x;
        assert.eq(dates[index].valueOf(), date.valueOf());
        index++;
    }
}

t.drop();
dates = [new Date(-5000000000000), new Date(5000000000000), new Date(0), new Date(5), new Date(-5)];
for (var i = 0; i < dates.length; i++) {
    t.insert({x:dates[i]});
}
dates.sort(function(a,b){return a - b});
reverseDates = dates.slice(0).reverse()

checkSorting(dates, 1)
checkSorting(reverseDates, -1)
t.ensureIndex({x:1})
checkSorting(dates, 1)
checkSorting(reverseDates, -1)
t.dropIndexes()
t.ensureIndex({x:-1})
checkSorting(dates, 1)
checkSorting(reverseDates, -1)
