// signed dates check
t = db.sort10;

function checkSorting1(opts) {
    t.drop();
    t.insert({x: new Date(50000)});
    t.insert({x: new Date(-50)});
    var d = new Date(-50);
    for (var pass = 0; pass < 2; pass++) {
        assert(t.find().sort({x: 1})[0].x.valueOf() == d.valueOf());
        t.ensureIndex({x: 1}, opts);
        t.insert({x: new Date()});
    }
}

checkSorting1({});
checkSorting1({"background": true});

function checkSorting2(dates, sortOrder) {
    cur = t.find().sort({x: sortOrder});
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
    t.insert({x: dates[i]});
}
dates.sort(function(a, b) {
    return a - b;
});
reverseDates = dates.slice(0).reverse();

checkSorting2(dates, 1);
checkSorting2(reverseDates, -1);
t.ensureIndex({x: 1});
checkSorting2(dates, 1);
checkSorting2(reverseDates, -1);
t.dropIndexes();
t.ensureIndex({x: -1});
checkSorting2(dates, 1);
checkSorting2(reverseDates, -1);
