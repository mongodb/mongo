p = db.jstests_hint1;
p.drop();

p.save({ts: new Date(1), cls: "entry", verticals: "alleyinsider", live: true});
p.ensureIndex({ts: 1});

assert.eq(
    1,
    p.find(
         {live: true, ts: {$lt: new Date(1234119308272)}, cls: "entry", verticals: "alleyinsider"})
        .sort({ts: -1})
        .hint({ts: 1})
        .count());
