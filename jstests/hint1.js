
p = db.jstests_hint1;
p.drop();

p.save( { ts: new Date( 1 ), cls: "entry", verticals: "alleyinsider", live: true } );
p.ensureIndex( { ts: 1 } );

e = p.find( { live: true, ts: { $lt: new Date( 1234119308272 ) }, cls: "entry", verticals: "alleyinsider" } ).sort( { ts: -1 } ).hint( { ts: 1 } ).explain();
assert.eq(e.indexBounds.ts[0][0].getTime(), new Date(1234119308272).getTime(), "A");

//printjson(e);

assert.eq( /*just below min date is bool true*/true, e.indexBounds.ts[0][1], "B");

assert.eq(1, p.find({ live: true, ts: { $lt: new Date(1234119308272) }, cls: "entry", verticals: "alleyinsider" }).sort({ ts: -1 }).hint({ ts: 1 }).count());

