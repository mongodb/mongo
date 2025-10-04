// signed dates check
//
// @tags: [requires_fastcount]

let t = db.sort10;

function checkSorting1(opts) {
    t.drop();
    t.insert({x: new Date(50000)});
    t.insert({x: new Date(-50)});
    let d = new Date(-50);
    for (let pass = 0; pass < 2; pass++) {
        assert(t.find().sort({x: 1})[0].x.valueOf() == d.valueOf());
        t.createIndex({x: 1}, opts);
        t.insert({x: new Date()});
    }
}

checkSorting1({});
checkSorting1({"background": true});

function checkSorting2(dates, sortOrder) {
    let cur = t.find().sort({x: sortOrder});
    assert.eq(dates.length, cur.count(), "Incorrect number of results returned");
    let index = 0;
    while (cur.hasNext()) {
        let date = cur.next().x;
        assert.eq(dates[index].valueOf(), date.valueOf());
        index++;
    }
}

t.drop();
let dates = [new Date(-5000000000000), new Date(5000000000000), new Date(0), new Date(5), new Date(-5)];
for (let i = 0; i < dates.length; i++) {
    t.insert({x: dates[i]});
}
dates.sort(function (a, b) {
    return a - b;
});
let reverseDates = dates.slice(0).reverse();

checkSorting2(dates, 1);
checkSorting2(reverseDates, -1);
t.createIndex({x: 1});
checkSorting2(dates, 1);
checkSorting2(reverseDates, -1);
t.dropIndexes();
t.createIndex({x: -1});
checkSorting2(dates, 1);
checkSorting2(reverseDates, -1);
