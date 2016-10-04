t = db.jstests_group6;
t.drop();

for (i = 1; i <= 10; ++i) {
    t.save({i: new NumberLong(i), y: 1});
}

assert.eq.automsg(
    "55",
    "t.group( {key:'y', reduce:function(doc,out){ out.i += doc.i; }, initial:{i:0} } )[ 0 ].i");

t.drop();
for (i = 1; i <= 10; ++i) {
    if (i % 2 == 0) {
        t.save({i: new NumberLong(i), y: 1});
    } else {
        t.save({i: i, y: 1});
    }
}

assert.eq.automsg(
    "55",
    "t.group( {key:'y', reduce:function(doc,out){ out.i += doc.i; }, initial:{i:0} } )[ 0 ].i");

t.drop();
for (i = 1; i <= 10; ++i) {
    if (i % 2 == 1) {
        t.save({i: new NumberLong(i), y: 1});
    } else {
        t.save({i: i, y: 1});
    }
}

assert.eq.automsg(
    "55",
    "t.group( {key:'y', reduce:function(doc,out){ out.i += doc.i; }, initial:{i:0} } )[ 0 ].i");

assert.eq.automsg(
    "NumberLong(10)",
    "t.group( {$reduce: function(doc, prev) { prev.count += 1; }, initial: {count: new NumberLong(0) }} )[ 0 ].count");